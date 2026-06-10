// linux_ftp_client_cpp17.cpp
// ------------------------------------------------------------
// 一个教学型 Linux FTP 客户端，配合你前面的 FTP 服务器使用。
// 支持：
//   1. 控制连接连接服务器 2100 端口
//   2. USER / PASS 匿名登录
//   3. PASV 被动模式
//   4. LIST 获取文件列表
//   5. RETR 下载文件
//   6. STOR 上传文件
//   7. PWD / CWD / QUIT
//
// 编译：
//   g++ -std=c++17 -Wall -Wextra -O2 linux_ftp_client_cpp17.cpp -o ftp_client
//
// 运行：
//   ./ftp_client 127.0.0.1 2100
//
// 注意：
//   这是 Linux 版本，使用 POSIX socket API。
// ------------------------------------------------------------

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

// RAII 文件描述符封装。
// 作用：对象析构时自动 close(fd)，避免 socket 泄漏。
// 这和你服务端里的 UniqueFd 思路一致。
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

// 确保把 len 字节全部发送出去。
// send 不保证一次就发完，所以要循环发送。
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

static bool sendString(int fd, const std::string& s) {
    return sendAll(fd, s.data(), s.size());
}

// 从控制连接读取一行 FTP 响应。
// FTP 协议行结尾一般是 \r\n。
// 这里忽略 \r，读到 \n 结束。
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

// 建立 TCP 连接。
// host 可以是 127.0.0.1，也可以是域名。
// 这里为了配合 PASV，限制使用 IPv4。
static UniqueFd connectTcp(const std::string& host, int port) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* result = nullptr;
    std::string portText = std::to_string(port);

    int rc = ::getaddrinfo(host.c_str(), portText.c_str(), &hints, &result);
    if (rc != 0) {
        throw std::runtime_error(
            "getaddrinfo failed: " + std::string(gai_strerror(rc))
        );
    }

    UniqueFd fd;

    for (addrinfo* p = result; p != nullptr; p = p->ai_next) {
        int rawFd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (rawFd < 0) {
            continue;
        }

        fd.reset(rawFd);

        if (::connect(fd.get(), p->ai_addr, p->ai_addrlen) == 0) {
            ::freeaddrinfo(result);
            return fd;
        }
    }

    ::freeaddrinfo(result);

    throw std::runtime_error(
        "connect failed: " + std::string(std::strerror(errno))
    );
}

struct FtpReply {
    int code = 0;
    std::vector<std::string> lines;
};

// 判断某一行是否是多行响应的结束行。
// FTP 多行响应格式类似：
//   211-Features:
//    PASV
//   211 End
static bool isReplyCodeLine(const std::string& line, int code, char sep) {
    if (line.size() < 4) {
        return false;
    }

    if (line[0] != static_cast<char>('0' + code / 100)) {
        return false;
    }

    if (line[1] != static_cast<char>('0' + (code / 10) % 10)) {
        return false;
    }

    if (line[2] != static_cast<char>('0' + code % 10)) {
        return false;
    }

    return line[3] == sep;
}

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
        // FTP 被动模式的典型流程：
        //   1. 控制连接发送 PASV
        //   2. 解析服务器返回的数据端口
        //   3. 客户端主动连接这个数据端口
        //   4. 控制连接发送 LIST
        //   5. 数据连接接收目录数据
        //   6. 控制连接读取 226 完成响应
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

    void download(const std::string& remoteFile, const std::string& localFile) {
        UniqueFd dataFd = openPassiveDataConnection();

        FtpReply start = command("RETR " + remoteFile);
        if (start.code >= 400) {
            throw std::runtime_error("RETR failed");
        }

        std::ofstream out(localFile, std::ios::binary);
        if (!out) {
            throw std::runtime_error("cannot open local file for writing: " + localFile);
        }

        char buffer[128 * 1024];

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

    FtpReply readReply() {
        std::string line;

        if (!recvLine(controlFd_.get(), line)) {
            throw std::runtime_error("control connection closed");
        }

        if (
            line.size() < 3 ||
            !std::isdigit(static_cast<unsigned char>(line[0])) ||
            !std::isdigit(static_cast<unsigned char>(line[1])) ||
            !std::isdigit(static_cast<unsigned char>(line[2]))
        ) {
            throw std::runtime_error("bad FTP reply: " + line);
        }

        FtpReply reply;
        reply.code = std::stoi(line.substr(0, 3));
        reply.lines.push_back(line);

        if (verbose_) {
            std::cout << "< " << line << "\n";
        }

        // 处理 FTP 多行响应。
        // 例如：
        //   211-Features:
        //    PASV
        //    SIZE
        //   211 End
        if (line.size() >= 4 && line[3] == '-') {
            while (true) {
                if (!recvLine(controlFd_.get(), line)) {
                    throw std::runtime_error("control connection closed in multiline reply");
                }

                reply.lines.push_back(line);

                if (verbose_) {
                    std::cout << "< " << line << "\n";
                }

                if (isReplyCodeLine(line, reply.code, ' ')) {
                    break;
                }
            }
        }

        return reply;
    }

    FtpReply command(const std::string& cmd) {
        if (verbose_) {
            std::cout << "> " << cmd << "\n";
        }

        if (!sendString(controlFd_.get(), cmd + "\r\n")) {
            throw std::runtime_error("send command failed: " + cmd);
        }

        return readReply();
    }

    void requireCode(const FtpReply& reply, int expected, const std::string& what) {
        if (reply.code != expected) {
            std::ostringstream oss;
            oss << what << " expected " << expected << ", got " << reply.code;
            throw std::runtime_error(oss.str());
        }
    }

    UniqueFd openPassiveDataConnection() {
        FtpReply r = command("PASV");
        requireCode(r, 227, "PASV");

        std::string all;
        for (const auto& line : r.lines) {
            all += line;
            all += "\n";
        }

        // PASV 响应格式：
        //   227 Entering Passive Mode (h1,h2,h3,h4,p1,p2).
        //
        // 端口计算：
        //   port = p1 * 256 + p2
        auto left = all.find('(');
        auto right = all.find(')', left == std::string::npos ? 0 : left);

        if (left == std::string::npos || right == std::string::npos || right <= left) {
            throw std::runtime_error("cannot parse PASV reply: " + all);
        }

        std::string inside = all.substr(left + 1, right - left - 1);

        std::replace(inside.begin(), inside.end(), ',', ' ');

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

        std::ostringstream ip;
        ip << h1 << "." << h2 << "." << h3 << "." << h4;

        int port = p1 * 256 + p2;

        std::string dataHost = ip.str();

        // 如果服务器返回 0.0.0.0，就退回使用控制连接的 host。
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
// 注意：这个版本不支持带空格的文件名。
// 这是教学版客户端，先保持简单。
static std::vector<std::string> splitWords(const std::string& line) {
    std::istringstream iss(line);

    std::vector<std::string> parts;
    std::string s;

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