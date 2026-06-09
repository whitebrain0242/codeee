// linux_ftp_server_cpp17.cpp
// ------------------------------------------------------------
// 一个教学型 Linux FTP 服务器，基于 RFC959 的常用子集实现。
// 特性：
//   1. 控制连接监听 2100 端口
//   2. 多线程：每个客户端控制连接由一个线程处理
//   3. 支持 PASV 被动模式
//   4. 支持 LIST/NLST/PWD/CWD/CDUP/TYPE/SYST/FEAT/NOOP/QUIT
//   5. 支持 RETR 下载、STOR 上传、REST 断点续传
//   6. 支持 SIZE/MDTM/DELE/MKD/RMD/RNFR/RNTO
//   7. 使用 sendfile 优化大文件下载
//   8. 使用 RAII 管理文件描述符，降低资源泄漏风险
//   9. 限制访问根目录，防止 ../ 目录穿越
//
// 编译：
//   g++ -std=c++17 -O2 -Wall -Wextra -pthread linux_ftp_server_cpp17.cpp -o ftp_server
//
// 运行：
//   mkdir -p ./ftp_root
//   ./ftp_server ./ftp_root 2100
//
// 测试示例：
//   lftp -p 2100 ftp://127.0.0.1
//   curl ftp://127.0.0.1:2100/ --user anonymous:anything
//   curl -T local.txt ftp://127.0.0.1:2100/local.txt --user anonymous:anything
//   curl -o download.txt ftp://127.0.0.1:2100/local.txt --user anonymous:anything
// ------------------------------------------------------------

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

static std::string nowTime() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%F %T");
    return oss.str();
}

static void logInfo(const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    // 简单“界面美观”：用 ANSI 颜色区分日志级别。
    std::cout << "\033[1;32m[INFO]\033[0m " << nowTime() << "  " << msg << std::endl;
}

static void logWarn(const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    std::cout << "\033[1;33m[WARN]\033[0m " << nowTime() << "  " << msg << std::endl;
}

static void logError(const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    std::cerr << "\033[1;31m[ERR ]\033[0m " << nowTime() << "  " << msg << std::endl;
}

static std::string trim(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

static std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

static std::string baseName(const std::string& path) {
    auto pos = path.find_last_of("/\\");
    if (pos == std::string::npos) return path;
    return path.substr(pos + 1);
}

// RAII 文件描述符封装：对象析构时自动 close，避免忘记释放 socket/file fd。
class UniqueFd {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) : fd_(fd) {}
    ~UniqueFd() { reset(); }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept : fd_(other.fd_) {
        other.fd_ = -1;
    }

    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    int get() const { return fd_; }
    bool valid() const { return fd_ >= 0; }

    int release() {
        int tmp = fd_;
        fd_ = -1;
        return tmp;
    }

    void reset(int newFd = -1) {
        if (fd_ >= 0) ::close(fd_);
        fd_ = newFd;
    }

private:
    int fd_ = -1;
};

static bool sendAll(int fd, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

static bool sendString(int fd, const std::string& s) {
    return sendAll(fd, s.data(), s.size());
}

static bool recvLine(int fd, std::string& line) {
    line.clear();
    char c = '\0';
    while (true) {
        ssize_t n = ::recv(fd, &c, 1, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        if (c == '\n') break;
        if (c != '\r') line.push_back(c);
        if (line.size() > 8192) return false;
    }
    return true;
}

static bool pathInsideRoot(const fs::path& root, const fs::path& target) {
    std::error_code ec;
    fs::path rel = fs::relative(target, root, ec);
    if (ec) return false;
    for (const auto& part : rel) {
        if (part == "..") return false;
    }
    return !rel.is_absolute();
}

static std::string permString(fs::perms p, bool isDir) {
    std::string s;
    s += isDir ? 'd' : '-';
    s += (p & fs::perms::owner_read)  != fs::perms::none ? 'r' : '-';
    s += (p & fs::perms::owner_write) != fs::perms::none ? 'w' : '-';
    s += (p & fs::perms::owner_exec)  != fs::perms::none ? 'x' : '-';
    s += (p & fs::perms::group_read)  != fs::perms::none ? 'r' : '-';
    s += (p & fs::perms::group_write) != fs::perms::none ? 'w' : '-';
    s += (p & fs::perms::group_exec)  != fs::perms::none ? 'x' : '-';
    s += (p & fs::perms::others_read) != fs::perms::none ? 'r' : '-';
    s += (p & fs::perms::others_write)!= fs::perms::none ? 'w' : '-';
    s += (p & fs::perms::others_exec) != fs::perms::none ? 'x' : '-';
    return s;
}

static std::string formatListLine(const fs::directory_entry& entry) {
    std::error_code ec;
    auto st = entry.symlink_status(ec);
    bool isDir = entry.is_directory(ec);
    auto perms = st.permissions();
    uintmax_t size = isDir ? 0 : entry.file_size(ec);

    // FTP LIST 常见格式类似 UNIX 的 ls -l。
    // 很多客户端并不严格要求真实 owner/group，这里使用 ftp ftp 占位。
    std::ostringstream oss;
    oss << permString(perms, isDir) << " 1 ftp ftp ";
    oss << std::setw(12) << size << " ";

    auto ftime = entry.last_write_time(ec);
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    std::time_t tt = std::chrono::system_clock::to_time_t(sctp);
    std::tm tm{};
    localtime_r(&tt, &tm);
    oss << std::put_time(&tm, "%b %d %H:%M") << " ";
    oss << entry.path().filename().string() << "\r\n";
    return oss.str();
}

static std::string formatMdtmTime(const fs::path& p) {
    std::error_code ec;
    auto ftime = fs::last_write_time(p, ec);
    if (ec) return "";
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    std::time_t tt = std::chrono::system_clock::to_time_t(sctp);
    std::tm tm{};
    gmtime_r(&tt, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y%m%d%H%M%S");
    return oss.str();
}

class FtpSession {
public:
    FtpSession(int controlFd, fs::path root, std::string peer)
        : controlFd_(controlFd), root_(std::move(root)), peer_(std::move(peer)) {}

    void run() {
        logInfo("control connected: " + peer_);
        reply(220, "Simple Linux FTP Server ready.");

        std::string line;
        while (recvLine(controlFd_.get(), line)) {
            line = trim(line);
            if (line.empty()) continue;

            std::string cmd, arg;
            parseCommand(line, cmd, arg);
            logInfo(peer_ + " > " + cmd + (arg.empty() ? "" : " " + arg));

            try {
                if (cmd == "USER") handleUSER(arg);
                else if (cmd == "PASS") handlePASS(arg);
                else if (cmd == "SYST") reply(215, "UNIX Type: L8");
                else if (cmd == "FEAT") handleFEAT();
                else if (cmd == "OPTS") reply(200, "OPTS accepted.");
                else if (cmd == "NOOP") reply(200, "NOOP ok.");
                else if (cmd == "PWD" || cmd == "XPWD") handlePWD();
                else if (cmd == "CWD") handleCWD(arg);
                else if (cmd == "CDUP") handleCWD("..");
                else if (cmd == "TYPE") handleTYPE(arg);
                else if (cmd == "PASV") handlePASV();
                else if (cmd == "LIST") handleLIST(arg, false);
                else if (cmd == "NLST") handleLIST(arg, true);
                else if (cmd == "RETR") handleRETR(arg);
                else if (cmd == "STOR") handleSTOR(arg);
                else if (cmd == "REST") handleREST(arg);
                else if (cmd == "SIZE") handleSIZE(arg);
                else if (cmd == "MDTM") handleMDTM(arg);
                else if (cmd == "DELE") handleDELE(arg);
                else if (cmd == "MKD" || cmd == "XMKD") handleMKD(arg);
                else if (cmd == "RMD" || cmd == "XRMD") handleRMD(arg);
                else if (cmd == "RNFR") handleRNFR(arg);
                else if (cmd == "RNTO") handleRNTO(arg);
                else if (cmd == "QUIT") {
                    reply(221, "Goodbye.");
                    break;
                } else {
                    reply(502, "Command not implemented.");
                }
            } catch (const std::exception& e) {
                logWarn(peer_ + " command error: " + e.what());
                reply(550, std::string("Operation failed: ") + e.what());
            }
        }

        closePassive();
        logInfo("control disconnected: " + peer_);
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

    void parseCommand(const std::string& line, std::string& cmd, std::string& arg) {
        auto pos = line.find(' ');
        if (pos == std::string::npos) {
            cmd = upper(line);
            arg.clear();
        } else {
            cmd = upper(line.substr(0, pos));
            arg = trim(line.substr(pos + 1));
        }
    }

    void reply(int code, const std::string& msg) {
        std::ostringstream oss;
        oss << code << " " << msg << "\r\n";
        sendString(controlFd_.get(), oss.str());
        logInfo(peer_ + " < " + std::to_string(code) + " " + msg);
    }

    void replyRaw(const std::string& msg) {
        sendString(controlFd_.get(), msg);
        std::string oneLine = msg;
        for (char& c : oneLine) {
            if (c == '\r' || c == '\n') c = ' ';
        }
        logInfo(peer_ + " < " + oneLine);
    }

    void requireLogin() {
        if (!loggedIn_) throw std::runtime_error("please login first");
    }

    fs::path currentRealPath() const {
        std::error_code ec;
        fs::path p = fs::weakly_canonical(root_ / cwdRel_, ec);
        if (ec) return root_;
        return p;
    }

    // 将 FTP 虚拟路径转换为服务器真实路径。
    // 关键安全点：转换后必须仍在 root_ 内，防止客户端使用 ../../etc/passwd 越界访问。
    fs::path resolvePath(const std::string& ftpPath) {
        std::string arg = trim(ftpPath);
        fs::path combined;
        if (arg.empty()) {
            combined = root_ / cwdRel_;
        } else if (!arg.empty() && arg[0] == '/') {
            combined = root_ / arg.substr(1);
        } else {
            combined = root_ / cwdRel_ / arg;
        }

        std::error_code ec;
        fs::path normalized = fs::weakly_canonical(combined, ec);
        if (ec) {
            // weakly_canonical 对不存在的新文件通常也能处理；此处兜底做 lexically_normal。
            normalized = fs::absolute(combined).lexically_normal();
        }

        if (!pathInsideRoot(root_, normalized)) {
            throw std::runtime_error("path escapes FTP root");
        }
        return normalized;
    }

    std::string toVirtualPath(const fs::path& real) {
        std::error_code ec;
        fs::path rel = fs::relative(real, root_, ec);
        if (ec || rel.empty() || rel == ".") return "/";
        std::string s = rel.generic_string();
        if (s.empty() || s == ".") return "/";
        return "/" + s;
    }

    void handleUSER(const std::string&) {
        // 教学项目常采用匿名登录，避免陷入用户认证细节。
        // 真正生产环境应接入 PAM、系统用户或专门账号数据库。
        reply(331, "User name ok, need password.");
    }

    void handlePASS(const std::string&) {
        loggedIn_ = true;
        reply(230, "Login successful.");
    }

    void handleFEAT() {
        // 多行响应格式：第一行 code-，最后一行 code 空格。
        std::ostringstream oss;
        oss << "211-Features:\r\n"
            << " PASV\r\n"
            << " REST STREAM\r\n"
            << " SIZE\r\n"
            << " MDTM\r\n"
            << " UTF8\r\n"
            << "211 End\r\n";
        replyRaw(oss.str());
    }

    void handlePWD() {
        requireLogin();
        reply(257, "\"" + toVirtualPath(currentRealPath()) + "\" is current directory.");
    }

    void handleCWD(const std::string& arg) {
        requireLogin();
        fs::path target = resolvePath(arg);
        if (!fs::exists(target) || !fs::is_directory(target)) {
            reply(550, "Not a directory.");
            return;
        }
        std::error_code ec;
        fs::path rel = fs::relative(target, root_, ec);
        if (ec || rel.empty()) rel = ".";
        cwdRel_ = rel;
        reply(250, "Directory changed to " + toVirtualPath(target));
    }

    void handleTYPE(const std::string& arg) {
        requireLogin();
        std::string a = upper(trim(arg));
        if (a == "I") {
            binaryMode_ = true;
            reply(200, "Type set to I.");
        } else if (a == "A") {
            binaryMode_ = false;
            reply(200, "Type set to A.");
        } else {
            reply(504, "Only TYPE I and TYPE A are supported.");
        }
    }

    static int createListenSocketOnPort(int port) {
        UniqueFd fd(::socket(AF_INET, SOCK_STREAM, 0));
        if (!fd.valid()) return -1;

        int opt = 1;
        setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(static_cast<uint16_t>(port));

        if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            return -1;
        }
        if (::listen(fd.get(), 1) < 0) {
            return -1;
        }
        return fd.release();
    }

    std::string localIpForPasv() {
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        if (::getsockname(controlFd_.get(), reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
            char buf[INET_ADDRSTRLEN]{};
            const char* p = ::inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf));
            if (p && std::string(p) != "0.0.0.0") return p;
        }
        return "127.0.0.1";
    }

    void handlePASV() {
        requireLogin();
        closePassive();

        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<int> dist(PASV_PORT_MIN, PASV_PORT_MAX);

        int fd = -1;
        int port = 0;
        for (int i = 0; i < 100; ++i) {
            port = dist(gen);
            fd = createListenSocketOnPort(port);
            if (fd >= 0) break;
        }
        if (fd < 0) {
            reply(421, "Cannot open passive port.");
            return;
        }

        pasvListenFd_.reset(fd);
        pasvPort_ = port;

        std::string ip = localIpForPasv();
        for (char& c : ip) {
            if (c == '.') c = ',';
        }
        int p1 = port / 256;
        int p2 = port % 256;

        // FTP PASV 响应格式：227 Entering Passive Mode (h1,h2,h3,h4,p1,p2)
        // 客户端随后会主动连接服务器这个临时数据端口。
        std::ostringstream oss;
        oss << "Entering Passive Mode (" << ip << "," << p1 << "," << p2 << ").";
        reply(227, oss.str());
    }

    void closePassive() {
        pasvListenFd_.reset();
        pasvPort_ = 0;
    }

    UniqueFd acceptDataConnection() {
        if (!pasvListenFd_.valid()) {
            throw std::runtime_error("send PASV before data command");
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(pasvListenFd_.get(), &rfds);
        timeval tv{};
        tv.tv_sec = DATA_ACCEPT_TIMEOUT_SEC;
        tv.tv_usec = 0;

        int ret;
        do {
            ret = ::select(pasvListenFd_.get() + 1, &rfds, nullptr, nullptr, &tv);
        } while (ret < 0 && errno == EINTR);

        if (ret <= 0) {
            closePassive();
            throw std::runtime_error("data connection timeout");
        }

        sockaddr_in cli{};
        socklen_t len = sizeof(cli);
        int dataFd = ::accept(pasvListenFd_.get(), reinterpret_cast<sockaddr*>(&cli), &len);
        closePassive(); // PASV 数据监听端口只服务一次传输，用完立即关闭。

        if (dataFd < 0) throw std::runtime_error("accept data connection failed");

        int opt = 1;
        setsockopt(dataFd, IPPROTO_TCP, 1 /* TCP_NODELAY */, &opt, sizeof(opt));
        return UniqueFd(dataFd);
    }

    void handleLIST(const std::string& arg, bool namesOnly) {
        requireLogin();
        fs::path target = resolvePath(arg);
        if (!fs::exists(target)) {
            reply(550, "Path not found.");
            return;
        }

        reply(150, "Opening ASCII mode data connection for file list.");
        UniqueFd dataFd = acceptDataConnection();

        std::ostringstream listing;
        if (fs::is_directory(target)) {
            for (const auto& entry : fs::directory_iterator(target)) {
                if (namesOnly) {
                    listing << entry.path().filename().string() << "\r\n";
                } else {
                    listing << formatListLine(entry);
                }
            }
        } else {
            if (namesOnly) listing << target.filename().string() << "\r\n";
            else listing << formatListLine(fs::directory_entry(target));
        }

        std::string data = listing.str();
        sendString(dataFd.get(), data);
        reply(226, "Transfer complete.");
    }

    void handleRETR(const std::string& arg) {
        requireLogin();
        if (arg.empty()) {
            reply(501, "Missing file name.");
            return;
        }

        fs::path file = resolvePath(arg);
        if (!fs::exists(file) || !fs::is_regular_file(file)) {
            reply(550, "File not found.");
            return;
        }

        uintmax_t fileSize = fs::file_size(file);
        if (restOffset_ > fileSize) {
            restOffset_ = 0;
            reply(554, "REST offset is larger than file size.");
            return;
        }

        UniqueFd fileFd(::open(file.c_str(), O_RDONLY));
        if (!fileFd.valid()) {
            reply(550, "Cannot open file.");
            return;
        }

        reply(150, "Opening BINARY mode data connection for download.");
        UniqueFd dataFd = acceptDataConnection();

        // 大文件下载优化：sendfile 在内核态直接把文件页发送到 socket，减少用户态拷贝。
        off_t offset = static_cast<off_t>(restOffset_);
        uintmax_t remain = fileSize - restOffset_;
        bool ok = true;
        while (remain > 0) {
            size_t chunk = static_cast<size_t>(std::min<uintmax_t>(remain, 16ull * 1024 * 1024));
            ssize_t n = ::sendfile(dataFd.get(), fileFd.get(), &offset, chunk);
            if (n < 0) {
                if (errno == EINTR) continue;
                ok = false;
                break;
            }
            if (n == 0) break;
            remain -= static_cast<uintmax_t>(n);
        }

        restOffset_ = 0;
        if (ok) reply(226, "Transfer complete.");
        else reply(426, "Connection closed; transfer aborted.");
    }

    void handleSTOR(const std::string& arg) {
        requireLogin();
        if (arg.empty()) {
            reply(501, "Missing file name.");
            return;
        }

        fs::path file = resolvePath(arg);
        fs::path parent = file.parent_path();
        if (!fs::exists(parent) || !fs::is_directory(parent)) {
            reply(550, "Parent directory does not exist.");
            return;
        }

        int flags = O_WRONLY | O_CREAT;
        if (restOffset_ == 0) flags |= O_TRUNC;
        UniqueFd fileFd(::open(file.c_str(), flags, 0666));
        if (!fileFd.valid()) {
            reply(550, "Cannot create file.");
            return;
        }

        if (restOffset_ > 0) {
            // 上传断点续传：REST n 后 STOR file，服务器把写指针移动到 n 处继续写入。
            if (::lseek(fileFd.get(), static_cast<off_t>(restOffset_), SEEK_SET) < 0) {
                restOffset_ = 0;
                reply(550, "Cannot seek file.");
                return;
            }
        }

        reply(150, "Opening BINARY mode data connection for upload.");
        UniqueFd dataFd = acceptDataConnection();

        std::vector<char> buffer(IO_BUFFER_SIZE);
        bool ok = true;
        while (true) {
            ssize_t n = ::recv(dataFd.get(), buffer.data(), buffer.size(), 0);
            if (n < 0) {
                if (errno == EINTR) continue;
                ok = false;
                break;
            }
            if (n == 0) break;

            ssize_t written = 0;
            while (written < n) {
                ssize_t m = ::write(fileFd.get(), buffer.data() + written, static_cast<size_t>(n - written));
                if (m < 0) {
                    if (errno == EINTR) continue;
                    ok = false;
                    break;
                }
                written += m;
            }
            if (!ok) break;
        }

        restOffset_ = 0;
        if (ok) reply(226, "Transfer complete.");
        else reply(426, "Connection closed; transfer aborted.");
    }

    void handleREST(const std::string& arg) {
        requireLogin();
        try {
            size_t idx = 0;
            unsigned long long off = std::stoull(trim(arg), &idx);
            if (idx != trim(arg).size()) throw std::invalid_argument("bad offset");
            restOffset_ = static_cast<uint64_t>(off);
            reply(350, "Restart position accepted.");
        } catch (...) {
            reply(501, "Bad REST offset.");
        }
    }

    void handleSIZE(const std::string& arg) {
        requireLogin();
        fs::path p = resolvePath(arg);
        if (!fs::exists(p) || !fs::is_regular_file(p)) {
            reply(550, "File not found.");
            return;
        }
        reply(213, std::to_string(fs::file_size(p)));
    }

    void handleMDTM(const std::string& arg) {
        requireLogin();
        fs::path p = resolvePath(arg);
        if (!fs::exists(p)) {
            reply(550, "Path not found.");
            return;
        }
        std::string t = formatMdtmTime(p);
        if (t.empty()) reply(550, "Cannot get modification time.");
        else reply(213, t);
    }

    void handleDELE(const std::string& arg) {
        requireLogin();
        fs::path p = resolvePath(arg);
        if (!fs::exists(p) || !fs::is_regular_file(p)) {
            reply(550, "File not found.");
            return;
        }
        std::error_code ec;
        fs::remove(p, ec);
        if (ec) reply(550, "Delete failed.");
        else reply(250, "File deleted.");
    }

    void handleMKD(const std::string& arg) {
        requireLogin();
        if (arg.empty()) {
            reply(501, "Missing directory name.");
            return;
        }
        fs::path p = resolvePath(arg);
        std::error_code ec;
        fs::create_directory(p, ec);
        if (ec) reply(550, "Create directory failed.");
        else reply(257, "\"" + toVirtualPath(p) + "\" created.");
    }

    void handleRMD(const std::string& arg) {
        requireLogin();
        fs::path p = resolvePath(arg);
        if (!fs::exists(p) || !fs::is_directory(p)) {
            reply(550, "Directory not found.");
            return;
        }
        std::error_code ec;
        fs::remove(p, ec);
        if (ec) reply(550, "Remove directory failed. Directory may not be empty.");
        else reply(250, "Directory removed.");
    }

    void handleRNFR(const std::string& arg) {
        requireLogin();
        fs::path p = resolvePath(arg);
        if (!fs::exists(p)) {
            reply(550, "Path not found.");
            return;
        }
        renameFrom_ = p;
        reply(350, "RNFR accepted; send RNTO.");
    }

    void handleRNTO(const std::string& arg) {
        requireLogin();
        if (renameFrom_.empty()) {
            reply(503, "Send RNFR first.");
            return;
        }
        fs::path to = resolvePath(arg);
        std::error_code ec;
        fs::rename(renameFrom_, to, ec);
        renameFrom_.clear();
        if (ec) reply(550, "Rename failed.");
        else reply(250, "Rename successful.");
    }
};

static UniqueFd createControlListenSocket(int port) {
    UniqueFd fd(::socket(AF_INET, SOCK_STREAM, 0));
    if (!fd.valid()) {
        throw std::runtime_error("socket failed: " + std::string(std::strerror(errno)));
    }

    int opt = 1;
    setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        throw std::runtime_error("bind failed: " + std::string(std::strerror(errno)));
    }

    if (::listen(fd.get(), BACKLOG) < 0) {
        throw std::runtime_error("listen failed: " + std::string(std::strerror(errno)));
    }

    return fd;
}

static std::string peerToString(const sockaddr_in& addr) {
    char ip[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    std::ostringstream oss;
    oss << ip << ":" << ntohs(addr.sin_port);
    return oss.str();
}

int main(int argc, char* argv[]) {
    // 避免客户端断开时 send 触发 SIGPIPE 导致服务器进程退出。
    std::signal(SIGPIPE, SIG_IGN);

    fs::path root = "./ftp_root";
    int port = DEFAULT_CONTROL_PORT;

    if (argc >= 2) root = argv[1];
    if (argc >= 3) port = std::stoi(argv[2]);

    try {
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

        logInfo("server started, waiting for clients...");

        while (true) {
            sockaddr_in cli{};
            socklen_t len = sizeof(cli);
            int clientFd = ::accept(listenFd.get(), reinterpret_cast<sockaddr*>(&cli), &len);
            if (clientFd < 0) {
                if (errno == EINTR) continue;
                logError("accept failed: " + std::string(std::strerror(errno)));
                continue;
            }

            std::string peer = peerToString(cli);

            // 每个控制连接一个线程：控制连接生命周期较长，且每个会话都有独立状态，
            // 例如当前目录、REST 偏移量、PASV 监听 fd 等。
            std::thread([clientFd, root, peer]() mutable {
                FtpSession session(clientFd, root, peer);
                session.run();
            }).detach();
        }
    } catch (const std::exception& e) {
        logError(e.what());
        return 1;
    }

    return 0;
}
