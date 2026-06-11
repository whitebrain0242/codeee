#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <random>
#include <sstream>
#include <string>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

static constexpr int DEFAULT_CONTROL_PORT = 2100;
static constexpr int BACKLOG = 64;
static constexpr int PASV_PORT_MIN = 50000;
static constexpr int PASV_PORT_MAX = 51000;
static constexpr int DATA_ACCEPT_TIMEOUT_SEC = 30;
static constexpr size_t IO_BUFFER_SIZE = 256 * 1024;

static std::mutex g_logMutex;

#define LOG(level, color, msg)                                                             \
    do                                                                                     \
    {                                                                                      \
        std::lock_guard<std::mutex> lock(g_logMutex);                                      \
        std::cout << "\033[" << color << "m" << level << "\033[0m   " << msg << std::endl; \
    } while (0)
#define LOG_INFO(msg) LOG("INFO", "1;32", msg)
#define LOG_WARN(msg) LOG("WARN", "1;33", msg)
#define LOG_ERROR(msg) LOG("ERR ", "1;31", msg)

// 2.字符串处理
// 去掉前后空格从前到后，从后到前
static std::string trim(const std::string &s)
{
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        --e;
    return s.substr(b, e - b);
}
// 变成大写
static std::string upper(std::string s)
{
    for (char &c : s)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

class UniqueFd
{
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) : fd_(fd) {}
    ~UniqueFd() { reset(); }

    UniqueFd(const UniqueFd &) = delete;
    UniqueFd &operator=(const UniqueFd &) = delete;

    // 移动构造——转移所有权
    UniqueFd(UniqueFd &&other) noexcept : fd_(other.fd_)
    {
        other.fd_ = -1;
    }

    int release()
    {
        int tmp = fd_;
        fd_ = -1;
        return tmp;
    }
    // 接管另外一个fd,原来的作废
    UniqueFd &operator=(UniqueFd &&other) noexcept
    {
        if (this != &other)
        {
            reset();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    int get() const { return fd_; }
    bool valid() const { return fd_ >= 0; }

    // 交出所有权
    void reset(int newFd = -1)
    {
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = newFd;
    }

private:
    int fd_ = -1;
};
// 3.网络收发
static bool sendAll(int fd, const char *data, size_t len)
{
    size_t sent = 0;
    while (sent < len)
    {
        ssize_t n = ::send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (n == 0)
            return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

static bool sendString(int fd, const std::string &s)
{
    return sendAll(fd, s.data(), s.size());
}
// 逐行读取命令，一个一个字符录入
static bool recvLine(int fd, std::string &line)
{
    line.clear();
    char c = '\0';
    while (true)
    {
        ssize_t n = ::recv(fd, &c, 1, 0);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (n == 0)
            return false;
        if (c == '\n')
            break;
        if (c != '\r')
            line.push_back(c);
        if (line.size() > 8192)
            return false;
    }
    return true;
}
/*安全相关*/
// 1.判断是否在根目录底下
static bool pathInsideRoot(const fs::path &root, const fs::path &target)
{
    std::error_code ec;
    fs::path rel = fs::relative(target, root, ec);
    if (ec)
        return false;
    for (const auto &part : rel)
    {
        if (part == "..")
            return false;
    }
    return !rel.is_absolute();
}
// 权限字符串
static std::string permString(fs::perms p, bool isDir)
{
    std::string s;
    s += isDir ? 'd' : '-';
    s += (p & fs::perms::owner_read) != fs::perms::none ? 'r' : '-';
    s += (p & fs::perms::owner_write) != fs::perms::none ? 'w' : '-';
    s += (p & fs::perms::owner_exec) != fs::perms::none ? 'x' : '-';
    s += (p & fs::perms::group_read) != fs::perms::none ? 'r' : '-';
    s += (p & fs::perms::group_write) != fs::perms::none ? 'w' : '-';
    s += (p & fs::perms::group_exec) != fs::perms::none ? 'x' : '-';
    s += (p & fs::perms::others_read) != fs::perms::none ? 'r' : '-';
    s += (p & fs::perms::others_write) != fs::perms::none ? 'w' : '-';
    s += (p & fs::perms::others_exec) != fs::perms::none ? 'x' : '-';
    return s;
}
// 把单个文件信息转换成ls-l传给客户端
static std::string formatListLine(const fs::directory_entry &entry)
{
    // 文件大小
    std::error_code ec;
    auto st = entry.symlink_status(ec);
    bool isDir = entry.is_directory(ec);
    auto perms = st.permissions();
    uintmax_t size = isDir ? 0 : entry.file_size(ec);

    // FTP LIST 常见格式类似 UNIX 的 ls -l。
    // 很多客户端并不严格要求真实 owner/group，这里使用 ftp ftp 占位。
    std::ostringstream oss;
    // 权限
    oss << permString(perms, isDir) << " 1 ftp ftp ";
    oss << std::setw(12) << size << " ";

    oss << entry.path().filename().string() << "\r\n";
    return oss.str();
}
// 存储客户端信息，进行客户端操作
class FtpSession
{
public:
    FtpSession(int controlFd, fs::path root, std::string peer)
        : controlFd_(controlFd), root_(std::move(root)), peer_(std::move(peer)) {}

    void run()
    {
        LOG_INFO("control connected: " + peer_);
        reply(220, "Simple Linux FTP Server ready.");

        std::string line;
        while (recvLine(controlFd_.get(), line))
        {
            line = trim(line);
            if (line.empty())
                continue;

            std::string cmd, arg;
            parseCommand(line, cmd, arg);
            LOG_INFO(peer_ + " > " + cmd + (arg.empty() ? "" : " " + arg));

            try
            {
                if (cmd == "USER")
                    handleUSER(arg);
                else if (cmd == "PASS")
                    handlePASS(arg);
                else if (cmd == "PWD" || cmd == "XPWD")
                    handlePWD();
                else if (cmd == "CWD")
                    handleCWD(arg);
                else if (cmd == "TYPE")
                    handleTYPE(arg);
                else if (cmd == "PASV")
                    handlePASV();
                else if (cmd == "LIST")
                    handleLIST(arg);
                else if (cmd == "RETR")
                    handleRETR(arg);
                else if (cmd == "STOR")
                    handleSTOR(arg);
                else if (cmd == "REST")
                    handleREST(arg);
                else if (cmd == "QUIT")
                {
                    reply(221, "Goodbye.");
                    break;
                }
                else
                {
                    reply(502, "Command not implemented.");
                }
            }
            catch (const std::exception &e)
            {
                LOG_WARN(peer_ + " command error: " + e.what());
                reply(550, std::string("Operation failed: ") + e.what());
            }
        }

        closePassive();
        LOG_INFO("control disconnected: " + peer_);
    }

private:
    UniqueFd controlFd_;
    fs::path root_;
    fs::path cwdRel_ = fs::path(".");
    std::string peer_;
    bool loggedIn_ = false;
    bool binaryMode_ = true;
    uint64_t restOffset_ = 0;
    fs::path renameFrom_;

    UniqueFd pasvListenFd_;
    int pasvPort_ = 0;

    // 2.虚拟路径转为绝对路径
    fs::path resolvePath(const std::string &ftpPath)
    {
        std::string arg = trim(ftpPath);
        fs::path combined;
        if (arg.empty())
        {
            combined = root_ / cwdRel_;
        }
        else if (!arg.empty() && arg[0] == '/')
        {
            combined = root_ / arg.substr(1);
        }
        else
        {
            combined = root_ / cwdRel_ / arg;
        }

        std::error_code ec;
        fs::path normalized = fs::weakly_canonical(combined, ec);
        if (ec)
        {
            normalized = fs::absolute(combined).lexically_normal();
        }

        if (!pathInsideRoot(root_, normalized))
        {
            throw std::runtime_error("path escapes FTP root");
        }
        return normalized;
    }
    // 拆分命令，复制cmd和arg
    void parseCommand(const std::string &line, std::string &cmd, std::string &arg)
    {
        auto pos = line.find(' ');
        if (pos == std::string::npos)
        {
            cmd = upper(line);
            arg.clear();
        }
        else
        {
            cmd = upper(line.substr(0, pos));
            arg = trim(line.substr(pos + 1));
        }
    }
    // 给客户端回复应答语句
    void reply(int code, const std::string &msg)
    {
        std::ostringstream oss;
        oss << code << " " << msg << "\r\n";
        sendString(controlFd_.get(), oss.str());
        LOG_INFO(peer_ + " < " + std::to_string(code) + " " + msg);
    }

    void requireLogin()
    {
        if (!loggedIn_)
            throw std::runtime_error("please login first");
    }

    // 获取绝对路径
    fs::path currentRealPath() const
    {
        std::error_code ec;
        fs::path p = fs::weakly_canonical(root_ / cwdRel_, ec);
        if (ec)
            return root_;
        return p;
    }
    // 获取虚拟路径
    std::string toVirtualPath(const fs::path &real)
    {
        std::error_code ec;
        fs::path rel = fs::relative(real, root_, ec);
        if (ec || rel.empty() || rel == ".")
            return "/";
        std::string s = rel.generic_string();
        if (s.empty() || s == ".")
            return "/";
        return "/" + s;
    }
    // FTP协议函数
    // 1.登陆

    void handleUSER(const std::string &)
    {
        reply(331, "User name ok, need password.");
    }

    void handlePASS(const std::string &)
    {
        loggedIn_ = true;
        reply(230, "Login successful.");
    }
    // 2.目录
    // 展示当前工作目录
    void handlePWD()
    {
        requireLogin();
        reply(257, "\"" + toVirtualPath(currentRealPath()) + "\" is current directory.");
    }

    // 切换工作目录
    void handleCWD(const std::string &arg)
    {
        requireLogin();
        fs::path target = resolvePath(arg);
        if (!fs::exists(target) || !fs::is_directory(target))
        {
            reply(550, "Not a directory.");
            return;
        }
        std::error_code ec;
        fs::path rel = fs::relative(target, root_, ec);
        if (ec || rel.empty())
            rel = ".";
        cwdRel_ = rel;
        reply(250, "Directory changed to " + toVirtualPath(target));
    }

    // 3.文件列表类
    void handleLIST(const std::string &arg)
    {
        requireLogin();
        fs::path target = resolvePath(arg);
        if (!fs::exists(target))
        {
            reply(550, "Path not found.");
            return;
        }

        reply(150, "Opening ASCII mode data connection for file list.");
        UniqueFd dataFd = acceptDataConnection();

        std::ostringstream listing;
        if (fs::is_directory(target))
        {
            for (const auto &entry : fs::directory_iterator(target))
            {
                listing << formatListLine(entry);
            }
        }
        else
        {
            listing << formatListLine(fs::directory_entry(target));
        }

        std::string data = listing.str();
        sendString(dataFd.get(), data);
        reply(226, "Transfer complete.");
    }

    // 切换数据传输形式：二进制或者ASC文本模式
    void handleTYPE(const std::string &arg)
    {
        requireLogin();
        std::string a = upper(trim(arg));
        if (a == "I")
        { // 二进制
            binaryMode_ = true;
            reply(200, "Type set to I.");
        }
        else if (a == "A")
        {
            binaryMode_ = false;
            reply(200, "Type set to A.");
        }
        else
        {
            reply(504, "Only TYPE I and TYPE A are supported.");
        }
    }

    // 创建客户端监听
    static int createListenSocketOnPort(int port)
    {
        UniqueFd fd(::socket(AF_INET, SOCK_STREAM, 0));
        if (!fd.valid())
            return -1;

        int opt = 1;
        setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(static_cast<uint16_t>(port));

        if (::bind(fd.get(), reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
        {
            return -1;
        }
        if (::listen(fd.get(), 1) < 0)
        {
            return -1;
        }
        return fd.release();
    }

    // 获取本地的ip地址，用于PASV
    std::string localIpForPasv()
    {
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        // 使用getsockname从已经建立的控制连皆中获取本端socket的地址信息存入addr
        if (::getsockname(controlFd_.get(), reinterpret_cast<sockaddr *>(&addr), &len) == 0)
        {
            char buf[INET_ADDRSTRLEN]{};
            const char *p = ::inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf));
            if (p && std::string(p) != "0.0.0.0")
                return p;
        }
        return "127.0.0.1";
    }

    void handlePASV()
    {
        requireLogin();
        closePassive();

        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<int> dist(PASV_PORT_MIN, PASV_PORT_MAX);

        int fd = -1;
        int port = 0;
        // 循环尝试创建监听socket
        for (int i = 0; i < 100; ++i)
        {
            port = dist(gen);
            fd = createListenSocketOnPort(port);
            if (fd >= 0)
                break; // 成功哦
        }
        if (fd < 0)
        {
            reply(421, "Cannot open passive port.");
            return;
        }

        pasvListenFd_.reset(fd);
        pasvPort_ = port;

        std::string ip = localIpForPasv();
        for (char &c : ip)
        {
            if (c == '.')
                c = ',';
        }
        // 计算端口的高字节 p1 = port / 256 和低字节 p2 = port % 256
        int p1 = port / 256;
        int p2 = port % 256;

        // FTP PASV 响应格式：227 Entering Passive Mode (h1,h2,h3,h4,p1,p2)
        std::ostringstream oss;
        oss << "Entering Passive Mode (" << ip << "," << p1 << "," << p2 << ").";
        reply(227, oss.str());
    }

    void closePassive()
    {
        pasvListenFd_.reset();
        pasvPort_ = 0;
    }

    // 等待并接受数据连接
    UniqueFd acceptDataConnection()
    {
        if (!pasvListenFd_.valid())
        {
            throw std::runtime_error("send PASV before data command");
        }

        // select需要fd和超市时间
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(pasvListenFd_.get(), &rfds);
        timeval tv{};
        tv.tv_sec = DATA_ACCEPT_TIMEOUT_SEC;
        tv.tv_usec = 0;

        int ret;
        do
        {
            ret = ::select(pasvListenFd_.get() + 1, &rfds, nullptr, nullptr, &tv);
        } while (ret < 0 && errno == EINTR);

        if (ret <= 0)
        {
            closePassive();
            throw std::runtime_error("data connection timeout");
        }

        sockaddr_in cli{};
        socklen_t len = sizeof(cli);
        int dataFd = ::accept(pasvListenFd_.get(), reinterpret_cast<sockaddr *>(&cli), &len);
        closePassive(); // PASV 数据监听端口只服务一次传输，用完立即关闭。

        if (dataFd < 0)
            throw std::runtime_error("accept data connection failed");

        int opt = 1;
        setsockopt(dataFd, IPPROTO_TCP, 1 /* TCP_NODELAY */, &opt, sizeof(opt));
        return UniqueFd(dataFd);
    }

    void handleRETR(const std::string &arg)
    {
        requireLogin();
        if (arg.empty())
        {
            reply(501, "Missing file name.");
            return;
        }

        fs::path file = resolvePath(arg);
        if (!fs::exists(file) || !fs::is_regular_file(file))
        {
            reply(550, "File not found.");
            return;
        }

        uintmax_t fileSize = fs::file_size(file);
        if (restOffset_ > fileSize)
        {
            restOffset_ = 0;
            reply(554, "REST offset is larger than file size.");
            return;
        }

        UniqueFd fileFd(::open(file.c_str(), O_RDONLY));
        if (!fileFd.valid())
        {
            reply(550, "Cannot open file.");
            return;
        }

        reply(150, "Opening BINARY mode data connection for download.");
        UniqueFd dataFd = acceptDataConnection();

        // 大文件下载优化：sendfile 在内核态直接把文件页发送到 socket，减少用户态拷贝。
        off_t offset = static_cast<off_t>(restOffset_);
        uintmax_t remain = fileSize - restOffset_;
        bool ok = true;
        while (remain > 0)
        {
            size_t chunk = static_cast<size_t>(std::min<uintmax_t>(remain, 16ull * 1024 * 1024));
            ssize_t n = ::sendfile(dataFd.get(), fileFd.get(), &offset, chunk);
            if (n < 0)
            {
                if (errno == EINTR)
                    continue;
                ok = false;
                break;
            }
            if (n == 0)
                break;
            remain -= static_cast<uintmax_t>(n);
        }

        restOffset_ = 0;
        if (ok)
            reply(226, "Transfer complete.");
        else
            reply(426, "Connection closed; transfer aborted.");
    }

    void handleSTOR(const std::string &arg)
    {
        requireLogin();
        if (arg.empty())
        {
            reply(501, "Missing file name.");
            return;
        }

        fs::path file = resolvePath(arg);
        fs::path parent = file.parent_path();
        if (!fs::exists(parent) || !fs::is_directory(parent))
        {
            reply(550, "Parent directory does not exist.");
            return;
        }

        int flags = O_WRONLY | O_CREAT;
        if (restOffset_ == 0)
            flags |= O_TRUNC;
        UniqueFd fileFd(::open(file.c_str(), flags, 0666));
        if (!fileFd.valid())
        {
            reply(550, "Cannot create file.");
            return;
        }

        if (restOffset_ > 0)
        {
            // 上传断点续传：REST n 后 STOR file，服务器把写指针移动到 n 处继续写入。
            if (::lseek(fileFd.get(), static_cast<off_t>(restOffset_), SEEK_SET) < 0)
            {
                restOffset_ = 0;
                reply(550, "Cannot seek file.");
                return;
            }
        }

        reply(150, "Opening BINARY mode data connection for upload.");
        UniqueFd dataFd = acceptDataConnection();

        std::vector<char> buffer(IO_BUFFER_SIZE);
        bool ok = true;
        while (true)
        {
            ssize_t n = ::recv(dataFd.get(), buffer.data(), buffer.size(), 0);
            if (n < 0)
            {
                if (errno == EINTR)
                    continue;
                ok = false;
                break;
            }
            if (n == 0)
                break;

            ssize_t written = 0;
            while (written < n)
            {
                ssize_t m = ::write(fileFd.get(), buffer.data() + written, static_cast<size_t>(n - written));
                if (m < 0)
                {
                    if (errno == EINTR)
                        continue;
                    ok = false;
                    break;
                }
                written += m;
            }
            if (!ok)
                break;
        }

        restOffset_ = 0;
        if (ok)
            reply(226, "Transfer complete.");
        else
            reply(426, "Connection closed; transfer aborted.");
    }

    void handleREST(const std::string &arg)
    {
        requireLogin();
        try
        {
            size_t idx = 0;
            unsigned long long off = std::stoull(trim(arg), &idx);
            if (idx != trim(arg).size())
                throw std::invalid_argument("bad offset");
            restOffset_ = static_cast<uint64_t>(off);
            reply(350, "Restart position accepted.");
        }
        catch (...)
        {
            reply(501, "Bad REST offset.");
        }
    }
};

static UniqueFd createControlListenSocket(int port)
{
    UniqueFd fd(::socket(AF_INET, SOCK_STREAM, 0));
    if (!fd.valid())
    {
        throw std::runtime_error("socket failed: " + std::string(std::strerror(errno)));
    }

    int opt = 1;
    setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (::bind(fd.get(), reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
    {
        throw std::runtime_error("bind failed: " + std::string(std::strerror(errno)));
    }

    if (::listen(fd.get(), BACKLOG) < 0)
    {
        throw std::runtime_error("listen failed: " + std::string(std::strerror(errno)));
    }

    return fd;
}

static std::string peerToString(const sockaddr_in &addr)
{
    char ip[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    std::ostringstream oss;
    oss << ip << ":" << ntohs(addr.sin_port);
    return oss.str();
}

int main(int argc, char *argv[])
{

    std::signal(SIGPIPE, SIG_IGN);

    fs::path root = "./ftp_root";
    int port = DEFAULT_CONTROL_PORT;

    if (argc >= 2)
        root = argv[1];
    if (argc >= 3)
        port = std::stoi(argv[2]);

    try
    {
        std::error_code ec;
        fs::create_directories(root, ec);
        root = fs::canonical(root);

        UniqueFd listenFd = createControlListenSocket(port);

        std::ostringstream banner;
        banner << "\n"
               << "============================================================\n"
               << "  Simple Linux FTP Server\n"
               << "------------------------------------------------------------\n"
               << "  Control Port : " << port << "\n"
               << "  FTP Root     : " << root.string() << "\n"
               << "  PASV Ports   : " << PASV_PORT_MIN << "-" << PASV_PORT_MAX << "\n"
               << "============================================================\n";
        std::cout << banner.str() << std::endl;

        LOG_INFO("server started, waiting for clients...");

        while (true)
        {
            sockaddr_in cli{};
            socklen_t len = sizeof(cli);
            int clientFd = ::accept(listenFd.get(), reinterpret_cast<sockaddr *>(&cli), &len);
            if (clientFd < 0)
            {
                if (errno == EINTR)
                    continue;
                LOG_ERROR("accept failed: " + std::string(std::strerror(errno)));
                continue;
            }

            std::string peer = peerToString(cli);

            std::thread([clientFd, root, peer]() mutable
                        {
                FtpSession session(clientFd, root, peer);
                session.run(); })
                .detach();
        }
    }
    catch (const std::exception &e)
    {
        LOG_ERROR(e.what());
        return 1;
    }

    return 0;
}
