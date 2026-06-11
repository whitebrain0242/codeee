#include <arpa/inet.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <netdb.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

class UniqueFd {
public:
    UniqueFd() = default;

    explicit UniqueFd(int fd) : fd_(fd) {}

    ~UniqueFd() {
        reset();
    }

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

    int get() const {
        return fd_;
    }

    bool valid() const {
        return fd_ >= 0;
    }

    void reset(int newFd = -1) {
        if (fd_ >= 0) {
            ::close(fd_);
        }
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
            if (errno == EINTR) {
                continue;
            }
            return false;
        }

        if (n == 0) {
            return false;
        }

        sent += static_cast<size_t>(n);
    }

    return true;
}
//将加了/r/n的命令发送给服务器
static bool sendString(int fd, const std::string& s) {
    return sendAll(fd, s.data(), s.size());
}

static bool recvLine(int fd, std::string& line) {
    line.clear();
    char c = '\0';
    while (true) {
        ssize_t n = ::recv(fd, &c, 1, 0);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }

        if (n == 0) {
            return false;
        }

        if (c == '\n') {
            break;
        }

        if (c != '\r') {
            line.push_back(c);
        }

        if (line.size() > 8192) {
            return false;
        }
    }

    return true;
}

static UniqueFd connectTcp(const std::string& host, int port) {
    //指定希望得到的地址类型
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* result = nullptr;
    std::string portText = std::to_string(port);
//地址解析：一个域名对应多个IP                                  要求
    int rc = ::getaddrinfo(host.c_str(), portText.c_str(), &hints, &result);
    if (rc != 0) {
        throw std::runtime_error(
            "getaddrinfo failed: " + std::string(gai_strerror(rc))
        );
    }

    UniqueFd fd;
    //遍历地址列表，尝试创建socket
    for (addrinfo* p = result; p != nullptr; p = p->ai_next) {
        int rawFd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (rawFd < 0) {
            continue;
        }

        fd.reset(rawFd);

        if (::connect(fd.get(), p->ai_addr, p->ai_addrlen) == 0) {
            ::freeaddrinfo(result);//释放地列表
            return fd;
        }
    }

    ::freeaddrinfo(result);

    throw std::runtime_error(
        "connect failed: " + std::string(std::strerror(errno))
    );
}

struct FtpReply {
    int code = 0;//状态码和原始行
    std::vector<std::string> lines;
};


class FtpClient {
public:
    explicit FtpClient(bool verbose = true)
        : verbose_(verbose) {}

    void connectToServer(const std::string& host, int port) {
        serverHost_ = host;

        controlFd_ = connectTcp(host, port);

        FtpReply hello = readReply();
        requireCode(hello, 220, "server greeting");
    }

    //登陆：传入用户名和密码，然后发送命令获取服务端响应，如果是331就用户名成功，230密码成功
    void login(const std::string& user, const std::string& pass) {
        FtpReply r1 = command("USER " + user);
        // 有些 FTP 服务器 USER 后直接 230 登录成功。
        // 你的服务器是 331，需要继续 PASS。
        if (r1.code == 230) {
            return;
        }

        requireCode(r1, 331, "USER");

        FtpReply r2 = command("PASS " + pass);
        requireCode(r2, 230, "PASS");
    }

    std::string pwd() {
        FtpReply r = command("PWD");
        requireCode(r, 257, "PWD");

        if (r.lines.empty()) {
            return "";
        }

        return r.lines.back();
    }

    void cd(const std::string& path) {
        FtpReply r = command("CWD " + path);
        requireCode(r, 250, "CWD");
    }

    std::string list(const std::string& path = "") {
        
        UniqueFd dataFd = openPassiveDataConnection();

        std::string cmd = path.empty() ? "LIST" : "LIST " + path;

        FtpReply start = command(cmd);
        if (start.code >= 400) {
            throw std::runtime_error("LIST failed");
        }

        std::string listing;
        char buffer[64 * 1024];

        while (true) {
            ssize_t n = ::recv(dataFd.get(), buffer, sizeof(buffer), 0);

            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }

                throw std::runtime_error(
                    "receive list data failed: " + std::string(std::strerror(errno))
                );
            }

            if (n == 0) {
                break;
            }

            listing.append(buffer, buffer + n);
        }

        // 数据连接用完关闭。
        dataFd.reset();

        FtpReply done = readReply();
        requireCode(done, 226, "LIST completion");
        return listing;
    }
//                            服务器路径                          本地保存的文件名
    void download(const std::string& remoteFile, const std::string& localFile) {
        UniqueFd dataFd = openPassiveDataConnection();

        //开始下载
        FtpReply start = command("RETR " + remoteFile);
        if (start.code >= 400) {
            throw std::runtime_error("RETR failed");
        }

        //输出文件流，以二进制打开本地文件
        std::ofstream out(localFile, std::ios::binary);
        if (!out) {//无法创建或者写入
            throw std::runtime_error("cannot open local file for writing: " + localFile);
        }

        char buffer[128 * 1024];//128KB缓冲区

        while (true) {
            ssize_t n = ::recv(dataFd.get(), buffer, sizeof(buffer), 0);

            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }

                throw std::runtime_error(
                    "download recv failed: " + std::string(std::strerror(errno))
                );
            }

            if (n == 0) {
                break;
            }
            //将buffer前n的字节写进去
            out.write(buffer, n);

            if (!out) {
                throw std::runtime_error("write local file failed: " + localFile);
            }
        }

        out.close();

        // 下载完成，关闭数据连接，然后等待控制连接上的 226。
        dataFd.reset();

        FtpReply done = readReply();
        requireCode(done, 226, "RETR completion");
    }

    void upload(const std::string& localFile, const std::string& remoteFile) {
        //输入文件流，用来从磁盘里面读取数据
        std::ifstream in(localFile, std::ios::binary);
        if (!in) {
            throw std::runtime_error("cannot open local file for reading: " + localFile);
        }

        UniqueFd dataFd = openPassiveDataConnection();

        FtpReply start = command("STOR " + remoteFile);
        if (start.code >= 400) {
            throw std::runtime_error("STOR failed");
        }

        std::vector<char> buffer(128 * 1024);

        while (in) {
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            std::streamsize n = in.gcount();

            if (n > 0) {
                bool ok = sendAll(
                    dataFd.get(),
                    buffer.data(),
                    static_cast<size_t>(n)
                );

                if (!ok) {
                    throw std::runtime_error(
                        "upload send failed: " + std::string(std::strerror(errno))
                    );
                }
            }
        }

        // 这里非常关键：
        // 上传完成后必须关闭数据连接，
        // 服务器端 recv 返回 0，才知道客户端已经传完了。
        dataFd.reset();

        FtpReply done = readReply();
        requireCode(done, 226, "STOR completion");
    }

    void quit() {
        if (controlFd_.valid()) {
            command("QUIT");
            controlFd_.reset();
        }
    }

private:
    UniqueFd controlFd_;
    std::string serverHost_;
    bool verbose_ = true;

    //读取命令行，并且把它分开
    FtpReply readReply() {
        std::string line;

        if (!recvLine(controlFd_.get(), line)) {
            throw std::runtime_error("control connection closed");
        }

        //判断响应码
        if (
            line.size() < 3 ||
            !std::isdigit(static_cast<unsigned char>(line[0])) ||
            !std::isdigit(static_cast<unsigned char>(line[1])) ||
            !std::isdigit(static_cast<unsigned char>(line[2]))
        ) {
            throw std::runtime_error("bad FTP reply: " + line);
        }

        //分离响应吗和语句
        FtpReply reply;
        reply.code = std::stoi(line.substr(0, 3));
        reply.lines.push_back(line);

        if (verbose_) {//开启日志输出
            std::cout << "< " << line << "\n";
        }

        return reply;
    }
//向服务器发送命令
    FtpReply command(const std::string& cmd) {
        if (verbose_) {
            std::cout << "> " << cmd << "\n";
        }

        if (!sendString(controlFd_.get(), cmd + "\r\n")) {
            throw std::runtime_error("send command failed: " + cmd);
        }

        return readReply();
    }
//验证ftp响应吗是否符合预期    响应吗和响应文本      预期的响应吗         错误信息的描述文字
    void requireCode(const FtpReply& reply, int expected, const std::string& what) {
        if (reply.code != expected) {//如果实际上受到的响应吗和期望值不一样，错误处理
            std::ostringstream oss;
            oss << what << " expected " << expected << ", got " << reply.code;
            throw std::runtime_error(oss.str());
        }
    }
//在PASV形式中，通过数据连接，解析出数据连接的IP和端口，然后主动连接该端口，返回数据连接的socket
    UniqueFd openPassiveDataConnection() {
        FtpReply r = command("PASV");//服务器返回之后的响应吗加文本
        requireCode(r, 227, "PASV");

        if (r.lines.empty()) {
        throw std::runtime_error("empty PASV reply");
        }

        
        const std::string& replyLine = r.lines[0];   // 直接取第一行
        auto left = replyLine.find('(');
        auto right = replyLine.find(')', left); 

        if (left == std::string::npos || right == std::string::npos || right <= left) {
            throw std::runtime_error("cannot parse PASV reply: " + replyLine);
        }

        //得到服务器的四个字节，服务器开放的数据端口号
        std::string inside = replyLine.substr(left + 1, right - left - 1);
        //所有逗号替换成空格
        std::replace(inside.begin(), inside.end(), ',', ' ');

        //istringstream 允许我们使用 >> 运算符依次读取以空白分隔的六个整数，赋值给变量 h1~p2
        std::istringstream iss(inside);

        int h1 = 0;
        int h2 = 0;
        int h3 = 0;
        int h4 = 0;
        int p1 = 0;
        int p2 = 0;

        if (!(iss >> h1 >> h2 >> h3 >> h4 >> p1 >> p2)) {
            throw std::runtime_error("bad PASV numbers: " + inside);
        }

        //得到了数据连接的服务端ip和端口
        std::ostringstream ip;
        ip << h1 << "." << h2 << "." << h3 << "." << h4;
        int port = p1 * 256 + p2;
        std::string dataHost = ip.str();

        if (dataHost == "0.0.0.0") {
            dataHost = serverHost_;
        }

        if (verbose_) {
            std::cout << "# data connection: " << dataHost << ":" << port << "\n";
        }
        return connectTcp(dataHost, port);
    }
};

static std::string baseName(const std::string& path) {
    size_t pos = path.find_last_of("/\\");

    if (pos == std::string::npos) {
        return path;
    }

    return path.substr(pos + 1);
}

// 简单命令行切分。
static std::vector<std::string> splitWords(const std::string& line) {
    //输入字符串流
    std::istringstream iss(line);

    std::vector<std::string> parts;
    std::string s;

    //每次读取一个单词
    while (iss >> s) {
        parts.push_back(s);
    }

    return parts;
}

static void printHelp() {
    std::cout
        << "Commands:\n"
        << "  ls [remote_dir]              list remote files\n"
        << "  get <remote_file> [local]    download file\n"
        << "  put <local_file> [remote]    upload file\n"
        << "  pwd                          show remote directory\n"
        << "  cd <remote_dir>              change remote directory\n"
        << "  quit                         exit\n"
        << "  help                         show this help\n";
}

int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    int port = 2100;

    if (argc >= 2) {
        host = argv[1];
    }

    if (argc >= 3) {
        port = std::stoi(argv[2]);
    }

    try {
        FtpClient client(true);

        client.connectToServer(host, port);

        // 你的服务器允许匿名登录。
        client.login("anonymous", "anonymous@");

        std::cout << "Connected to FTP server " << host << ":" << port << "\n";

        printHelp();

        std::string line;

        while (true) {
            std::cout << "ftp> ";

            if (!std::getline(std::cin, line)) {
                break;
            }

            auto args = splitWords(line);

            if (args.empty()) {
                continue;
            }

            const std::string& cmd = args[0];

            try {
                if (cmd == "ls") {
                    std::string path = args.size() >= 2 ? args[1] : "";
                    std::cout << client.list(path);
                } else if (cmd == "get") {
                    if (args.size() < 2) {
                        std::cout << "usage: get <remote_file> [local_file]\n";
                        continue;
                    }

                    std::string remote = args[1];
                    std::string local = args.size() >= 3 ? args[2] : baseName(remote);

                    client.download(remote, local);

                    std::cout << "downloaded to " << local << "\n";
                } else if (cmd == "put") {
                    if (args.size() < 2) {
                        std::cout << "usage: put <local_file> [remote_file]\n";
                        continue;
                    }

                    std::string local = args[1];
                    std::string remote = args.size() >= 3 ? args[2] : baseName(local);

                    client.upload(local, remote);

                    std::cout << "uploaded as " << remote << "\n";
                } else if (cmd == "pwd") {
                    std::cout << client.pwd() << "\n";
                } else if (cmd == "cd") {
                    if (args.size() < 2) {
                        std::cout << "usage: cd <remote_dir>\n";
                        continue;
                    }

                    client.cd(args[1]);
                } else if (cmd == "quit" || cmd == "exit") {
                    client.quit();
                    break;
                } else if (cmd == "help") {
                    printHelp();
                } else {
                    std::cout << "unknown command: " << cmd << "\n";
                }
            } catch (const std::exception& e) {
                std::cerr << "command error: " << e.what() << "\n";
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    }

    return 0;
}