#include "file_transfer_service.hpp"

#include "minimuduo/net/TcpConnection.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <system_error>
#include <utility>

namespace {

constexpr std::size_t  =
    3072U;//一个数据块的大小
//把枚举类型转换称人类可读的字符串,用于日志输出和报文
std::string scope_text(
    chatroom::v9::FileTransferScope scope
) {
    switch (scope) {
        case chatroom::v9::FILE_TRANSFER_PRIVATE:
            return "PRIVATE";

        case chatroom::v9::FILE_TRANSFER_GROUP:
            return "GROUP";

        default:
            return "UNKNOWN";
    }
}
//验证元数据,一个一个比对
bool same_metadata_identity(
    const FileTransferMetadata& left,
    const FileTransferMetadata& right
) {
    return
        left.transfer_token() ==
            right.transfer_token() &&
        left.scope() ==
            right.scope() &&
        left.sender_username() ==
            right.sender_username() &&
        left.recipient_username() ==
            right.recipient_username() &&
        left.group_id() ==
            right.group_id() &&
        left.group_name() ==
            right.group_name() &&
        left.file_name() ==
            right.file_name() &&
        left.file_size() ==
            right.file_size() &&
        left.sha256_hex() ==
            right.sha256_hex();
}

}  // namespace

FileTransferService::FileTransferService(
    std::filesystem::path storage_root,
    std::size_t worker_count
)
    : storage_root_(
          std::move(storage_root)
      ),
      temp_root_(
          storage_root_ / "tmp"
      ),
      files_root_(
          storage_root_ / "files"
      ) {
        //确保至少有一个线程
    worker_count =
        std::max<std::size_t>(
            1U,
            worker_count
        );
    //预分配内存
    workers_.reserve(
        worker_count
    );
    //循环创建并启动工作线程
    for (std::size_t index = 0U;
         index < worker_count;
         ++index) {
        workers_.emplace_back(
            [this] {
                worker_loop();
            }
        );
    }
}

FileTransferService::~FileTransferService() {
    stop();
}
//在磁盘上创建两个目录
bool FileTransferService::initialize(
    std::string& error
) {
    std::error_code filesystem_error;

    std::filesystem::create_directories(
        temp_root_,
        filesystem_error
    );

    if (filesystem_error) {
        error =
            "cannot create file-transfer temp directory: " +
            filesystem_error.message();
        return false;
    }

    std::filesystem::create_directories(
        files_root_,
        filesystem_error
    );

    if (filesystem_error) {
        error =
            "cannot create file-transfer storage directory: " +
            filesystem_error.message();
        return false;
    }

    return true;
}
//仅打开磁盘,判断之前有没有上传过这个文件,若有,那就返回已有文件大小,没有,从头开始上传
bool FileTransferService::begin_or_resume_upload(
    const FileUploadResumeState& requested,//元数据
    FileUploadResumeState& persisted,//1.恢复上传--填充磁盘已有元数据2.新上传--复制一份元数据,做后续记录
    std::filesystem::path& temp_path,//本次上传的临时文件路径,后续追加的时候就是这个路径
    std::uint64_t& accepted_offset,//告诉客户端服务端已经受到了多少个字节1.恢复上传,返回临时文件的大小,客户端接着发2.新上传,返回0,客户端从头开始传输
    std::string& error
) {
    std::lock_guard<std::mutex> lock(
        upload_mutex_
    );
    //先看有没有元数据
    if (!requested.has_metadata()) {
        error =
            "upload resume state has no metadata";
        return false;
    }

    const FileTransferMetadata& metadata =requested.metadata();

    if (!fileutil::is_valid_transfer_token(
            metadata.transfer_token()
        )) {
        error =
            "invalid transfer token";
        return false;
    }
    //拼接磁盘路径.part
    temp_path =
        upload_part_path(
            metadata.transfer_token()
        );
    //.resume.pb
    const std::filesystem::path meta_path =
        upload_meta_path(
            metadata.transfer_token()
        );

    std::error_code filesystem_error;
    //检查文件是否曾经上传过
    const bool part_exists =
        std::filesystem::exists(
            temp_path,
            filesystem_error
        );

    if (filesystem_error) {
        error =
            "cannot inspect upload part file";
        return false;
    }

    filesystem_error.clear();

    const bool meta_exists =
        std::filesystem::exists(
            meta_path,
            filesystem_error
        );

    if (filesystem_error) {
        error =
            "cannot inspect upload resume metadata";
        return false;
    }
    //这两二个文件必须同时存在,断点续传
    if (part_exists && meta_exists) {
        FileUploadResumeState existing;
        
        if (!read_resume_state(
                meta_path,
                existing,
                error
            )) {
            return false;
        }
        //检查 token、发送者、文件名、大小、SHA 是否完全一致
        if (!same_resume_identity(
                requested,
                existing
            )) {
            error =
                "transfer token already belongs "
                "to different upload metadata";
            return false;
        }
        //获取.part磁盘文件大小
        const std::uint64_t current_size =
            static_cast<std::uint64_t>(
                std::filesystem::file_size(
                    temp_path,
                    filesystem_error
                )
            );

        if (filesystem_error) {
            error =
                "cannot inspect resumed upload size";
            return false;
        }
        //当前磁盘大小要小于声明的大小
        if (current_size >
            metadata.file_size()) {
            error =
                "server partial file is larger "
                "than declared file size";
            return false;
        }
        //赋值,代表是断点续传而不是新上传
        persisted =
            std::move(existing);
        //已经有的大小
        accepted_offset =
            current_size;

        return true;
    }

    //如果只有一个存在--清理并且重现开始--按照新文件处理
    if (part_exists || meta_exists) {
        filesystem_error.clear();
        std::filesystem::remove(
            temp_path,
            filesystem_error
        );

        filesystem_error.clear();
        std::filesystem::remove(
            meta_path,
            filesystem_error
        );
    }
    //两个文件都不存在
    {//创建一个全新的.part文件
        std::ofstream output(
            temp_path,
            std::ios::binary
        );

        if (!output) {
            error =
                "cannot create temporary upload file";
            return false;
        }
    }
    //将客户端传来的元数据序列化写入.resume.pb文件中
    if (!write_resume_state(
            meta_path,
            requested,
            error
        )) {
        //如果失败,删除刚才创建的.part文件,防止留下孤儿
        filesystem_error.clear();
        std::filesystem::remove(
            temp_path,
            filesystem_error
        );
        return false;
    }
    //标记为新上传
    persisted = requested;
    accepted_offset = 0U;//客户端受到0会从头开始传
    return true;
}
//内存到磁盘:将客户端发送的二进制数据块追加到.part文件上,确保连续性
bool FileTransferService::append_upload_chunk(
    const std::filesystem::path& temp_path,
    std::uint64_t expected_offset,
    const std::vector<unsigned char>& bytes,
    std::uint64_t& accepted_offset,
    std::string& error
) {
    std::lock_guard<std::mutex> lock(
        upload_mutex_
    );

    std::error_code filesystem_error;
    //获取当前文件大小
    const std::uint64_t current_size =
        static_cast<std::uint64_t>(
            std::filesystem::file_size(
                temp_path,
                filesystem_error
            )
        );

    if (filesystem_error) {
        error =
            "cannot inspect temporary upload file";
        return false;
    }
    //要求服务器记录的文件量和客户端的偏移量相同才可以进行下去
    if (current_size !=
        expected_offset) {
        error =
            "file chunk offset mismatch";
        return false;
    }
    //追加模式打开并且写入数据
    std::ofstream output(
        temp_path,
        std::ios::binary |
            std::ios::app
    );

    if (!output) {
        error =
            "cannot append temporary upload file";
        return false;
    }

    if (!bytes.empty()) {
        output.write(
            reinterpret_cast<const char*>(
                bytes.data()
            ),
            static_cast<std::streamsize>(
                bytes.size()
            )
        );
    }

    if (!output) {
        error =
            "failed while writing upload chunk";
        return false;
    }
    //赋值新的偏移量
    accepted_offset =
        current_size +
        static_cast<std::uint64_t>(
            bytes.size()
        );

    return true;
}
//传输玩后的验证和处理
bool FileTransferService::finalize_upload(
    const std::filesystem::path& temp_path,
    const std::string& token,
    const std::string& original_file_name,
    std::uint64_t expected_size,
    const std::string& expected_sha256_hex,
    std::string& stored_relative_path,
    std::string& error
) {
    std::lock_guard<std::mutex> lock(
        upload_mutex_
    );

    std::error_code filesystem_error;
    //获取磁盘文件大小
    const std::uint64_t actual_size =
        static_cast<std::uint64_t>(
            std::filesystem::file_size(
                temp_path,
                filesystem_error
            )
        );

    if (filesystem_error) {
        error =
            "cannot inspect completed upload";
        return false;
    }
    //检验大小正确与否
    if (actual_size !=
        expected_size) {
        error =
            "uploaded file size does not match "
            "FILE_BEGIN";
        return false;
    }

    std::string actual_sha256;
    //计算服务端磁盘文件哈希值
    if (!fileutil::sha256_file_hex(
            temp_path,
            actual_sha256,
            error
        )) {
        return false;
    }
    //客户端哈希值转换成字符串
    std::string expected =
        expected_sha256_hex;
    //哈希值转为小写
    std::transform(
        expected.begin(),
        expected.end(),
        expected.begin(),
        [](unsigned char character) {
            return static_cast<char>(
                std::tolower(character)
            );
        }
    );
    //判断二者是否相同
    if (actual_sha256 != expected) {
        error =
            "uploaded file SHA-256 mismatch";
        return false;
    }
    //最终文件名
    const std::string safe_name =
        fileutil::sanitize_filename(
            original_file_name
        );

    const std::filesystem::path final_name =
        token +
        "_" +
        safe_name;

    const std::filesystem::path final_path =
        files_root_ /
        final_name;
    //将临时文件移动到正式目录
    std::filesystem::rename(
        temp_path,
        final_path,
        filesystem_error
    );

    if (filesystem_error) {
        filesystem_error.clear();
        //如果失败了,复制临时文件到目标路径,成功后删除临时文件
        std::filesystem::copy_file(
            temp_path,
            final_path,
            std::filesystem::copy_options::
                overwrite_existing,
            filesystem_error
        );

        if (filesystem_error) {
            error =
                "cannot move completed file "
                "into storage: " +
                filesystem_error.message();
            return false;
        }

        filesystem_error.clear();
        std::filesystem::remove(
            temp_path,
            filesystem_error
        );
    }
    //删除元数据
    filesystem_error.clear();
    std::filesystem::remove(
        upload_meta_path(token),
        filesystem_error
    );
    //生成正式存储路径并返回,用于后续下载文件
    stored_relative_path =
        (
            std::filesystem::path("files") /
            final_name
        ).generic_string();

    return true;
}
//删除磁盘上的.part.resume.pb
void FileTransferService::cancel_upload(
    const std::string& token
) {
    if (!fileutil::is_valid_transfer_token(
            token
        )) {
        return;
    }

    std::lock_guard<std::mutex> lock(
        upload_mutex_
    );

    std::error_code ignored;

    std::filesystem::remove(
        upload_part_path(token),
        ignored
    );

    ignored.clear();

    std::filesystem::remove(
        upload_meta_path(token),
        ignored
    );
}

void FileTransferService::deliver_async(
    std::uint64_t transfer_id,
    const FileTransferMetadata& metadata,
    std::uint64_t start_offset,
    const minimuduo::net::TcpConnectionPtr& connection,
    CompletionCallback completion
) {//捕获所有的变量,mutable允许修改捕获的变量
    submit(
        [
            this,
            transfer_id,
            metadata,
            start_offset,
            connection,
            completion =
                std::move(completion)
        ]() mutable {
            //异步设计:没有直接发送,而是只负责解析请求并提交人物,置于执行交给了工作线程
            deliver_file(
                transfer_id,
                std::move(metadata),
                start_offset,
                connection,
                std::move(completion)
            );
        }
    );
}
//在文件向客户短发送之前,会先告诉客户端即将受到一个文件和元数据
std::string FileTransferService::make_offer_line(
    std::uint64_t transfer_id,
    const FileTransferMetadata& metadata
) const {
    //转化为字节序列,进行base64编码
    const std::vector<unsigned char>
        filename_bytes(
            metadata.file_name().begin(),
            metadata.file_name().end()
        );

    const std::vector<unsigned char>
        group_bytes(
            metadata.group_name().begin(),
            metadata.group_name().end()
        );

    const std::string encoded_group =
        group_bytes.empty()
            ? std::string("-")
            : fileutil::base64_encode(
                  group_bytes
              );

    return
        "FILE_OFFER " +
        std::to_string(
            transfer_id
        ) +
        " " +
        scope_text(
            metadata.scope()
        ) +
        " " +
        metadata.sender_username() +
        " " +
        encoded_group +
        " " +
        fileutil::base64_encode(
            filename_bytes
        ) +
        " " +
        std::to_string(
            metadata.file_size()
        ) +
        " " +
        metadata.sha256_hex() +
        "\n";
}

void FileTransferService::stop() {
    {
        std::lock_guard<std::mutex> lock(
            queue_mutex_
        );

        if (stopping_) {
            return;
        }

        stopping_ = true;
    }
    //唤醒所有工作线程
    queue_cv_.notify_all();
    //等待所有工作线程结束
    for (std::thread& worker :
         workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    //清除
    workers_.clear();

    std::queue<std::function<void()>>
        empty;

    {
        std::lock_guard<std::mutex> lock(
            queue_mutex_
        );
        tasks_.swap(empty);
    }
}
//后台工作线程的主循环
void FileTransferService::worker_loop() {
    while (true) {
        std::function<void()> task;

        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            //等待--如果已经停止或者是任务列表非空,释放锁挂起线程
            //等到条件变量侥幸线程的时候,重新获取锁
            queue_cv_.wait(lock, [this] {
                return stopping_ || !tasks_.empty();
            });
              
            if (stopping_ &&
                tasks_.empty()) {
                return;
            }
            //取出任务
            task =
                std::move(
                    tasks_.front()
                );
            //移除该任务
            tasks_.pop();
        }
        //解锁执行任务
        task();
    }
}
//把一个人物放进任务队列,唤醒一个线程去处理
void FileTransferService::submit(
    std::function<void()> task
) {
    {
        std::lock_guard<std::mutex> lock(
            queue_mutex_
        );

        if (stopping_) {
            return;
        }
        //人物入队,move不用拷贝移动效率更高
        tasks_.push(
            std::move(task)
        );
    }
    //条件变量叫醒
    queue_cv_.notify_one();
}
//客户端读取从磁盘中取出到内存的文件,send到socket
void FileTransferService::deliver_file(
    std::uint64_t transfer_id,
    FileTransferMetadata metadata,
    std::uint64_t start_offset,//偏移量
    minimuduo::net::TcpConnectionPtr connection,
    CompletionCallback completion
) {
    if (connection == nullptr ||
        !connection->connected()) {
        if (completion) {
            completion(
                false,
                "recipient connection is closed"
            );
        }
        return;
    }
    //偏移量是否大于文件大小
    if (start_offset >
        metadata.file_size()) {
        if (completion) {
            completion(
                false,
                "resume offset exceeds file size"
            );
        }
        return;
    }
    //打开磁盘文件
    const std::filesystem::path file_path =
        storage_root_ /
        metadata.stored_relative_path();

    std::ifstream input(
        file_path,
        std::ios::binary
    );

    if (!input) {
        if (completion) {
            completion(
                false,
                "stored file is missing"
            );
        }
        return;
    }
    //移动文件偏移量到指定位置
    input.seekg(
        static_cast<std::streamoff>(
            start_offset
        ),
        std::ios::beg//从头开始数
    );

    if (!input) {
        if (completion) {
            completion(
                false,
                "failed to seek stored file "
                "to resume offset"
            );
        }
        return;
    } 
    //发送文件传输开始通知
    connection->send(
        "FILE_RESUME_START " +
        std::to_string(transfer_id) +
        " " +
        std::to_string(start_offset) +
        "\n"
    );

    std::array<
        unsigned char,
        kFileChunkBytes
    > buffer{};

    std::uint64_t offset =
        start_offset;

    while (input) {
        //服务端磁盘到内存buffer
        input.read(
            reinterpret_cast<char*>(
                buffer.data()
            ),
            static_cast<std::streamsize>(
                buffer.size()
            )
        );
        //读取的字节数
        const std::streamsize count =
            input.gcount();

        if (count <= 0) {
            break;
        }
        //构造数据块
        std::vector<unsigned char> chunk(
            buffer.begin(),
            buffer.begin() + count
        );
        //二进制转字符串
        const std::string encoded =
            fileutil::base64_encode(
                chunk
            );
        //服务器内存向socket发送
        connection->send(
            "FILE_DATA " +
            std::to_string(
                transfer_id
            ) +
            " " +
            std::to_string(
                offset
            ) +
            " " +
            encoded +
            "\n"
        );

        offset +=
            static_cast<std::uint64_t>(
                count
            );
    }
    //若循环推出不是因为eof,回调失败
    if (!input.eof()) {
        if (completion) {
            completion(
                false,
                "failed while reading stored file"
            );
        }
        return;
    }
    //若最终累计的 offset 与 metadata.file_size() 不一致,例如中途文件大小变化,回调失败
    if (offset !=
        metadata.file_size()) {
        if (completion) {
            completion(
                false,
                "stored file size no longer "
                "matches metadata"
            );
        }
        return;
    }
    //告知客户端文件全部发送完毕
    //在改行数据写入TCp中时回调执行,通知上层业务文件传输完成
    connection->send(
        "FILE_DONE " +
        std::to_string(
            transfer_id
        ) +
        "\n",
        [
            completion =
                std::move(completion)
        ]() mutable {
            if (completion) {
                completion(
                    true,
                    {}
                );
            }
        }
    );
}
//不管文件在哪里,只需要上传token
//part,拼接磁盘路径
std::filesystem::path
FileTransferService::upload_part_path(
    const std::string& token
) const {
    return
        temp_root_ /
        (token + ".part");
}
//.resume.pb拼接磁盘路径
std::filesystem::path
FileTransferService::upload_meta_path(
    const std::string& token
) const {
    return
        temp_root_ /
        (token + ".resume.pb");
}
//序列化元数据
bool FileTransferService::write_resume_state(
    const std::filesystem::path& path,
    const FileUploadResumeState& state,
    std::string& error
) const {
    std::string bytes;
    //序列化元数据
    if (!state.SerializeToString(
            &bytes
        )) {
        error =
            "official protobuf failed to "
            "serialize FileUploadResumeState";
        return false;
    }
    //先写临时文件tmp
    const std::filesystem::path temp =
        path.string() + ".tmp";

    {
        std::ofstream output(
            temp,
            std::ios::binary |
                std::ios::trunc
        );

        if (!output) {
            error =
                "cannot create upload resume sidecar";
            return false;
        }

        output.write(
            bytes.data(),
            static_cast<std::streamsize>(
                bytes.size()
            )
        );

        if (!output) {
            error =
                "failed to write upload "
                "resume sidecar";
            return false;
        }
    }

    std::error_code filesystem_error;
    //rename写入磁盘
    std::filesystem::rename(
        temp,
        path,
        filesystem_error
    );

    if (filesystem_error) {
        filesystem_error.clear();

        std::filesystem::remove(
            path,
            filesystem_error
        );

        filesystem_error.clear();

        std::filesystem::rename(
            temp,
            path,
            filesystem_error
        );
    }

    if (filesystem_error) {
        error =
            "cannot finalize upload resume "
            "sidecar: " +
            filesystem_error.message();
        return false;
    }

    return true;
}
//读取磁盘上的元数据,反序列化称existing对象
bool FileTransferService::read_resume_state(
    const std::filesystem::path& path,
    FileUploadResumeState& state,
    std::string& error
) const {//打开文件
    std::ifstream input(
        path,
        std::ios::binary
    );

    if (!input) {
        error =
            "cannot open upload resume sidecar";
        return false;
    }
    //读取全部字节
    std::string bytes(
        (
            std::istreambuf_iterator<char>(
                input
            )
        ),
        std::istreambuf_iterator<char>()
    );

    if (!input.eof() &&
        input.fail()) {
        error =
            "failed to read upload "
            "resume sidecar";
        return false;
    }
    //反序列化回c++对象
    //为什么要序列化:是因为state是protobuf对象,里面有很多数据类型,序列化把字段变成二进制六
    if (!state.ParseFromString(
            bytes
        )) {
        error =
            "official protobuf failed to parse "
            "FileUploadResumeState";
        return false;
    }

    return true;
}
//不仅仅检查的是token,而且还检查其他的,身份验证不仅仅检查token
bool FileTransferService::same_resume_identity(
    const FileUploadResumeState& left,
    const FileUploadResumeState& right
) {
    if (!left.has_metadata() ||
        !right.has_metadata()) {
        return false;
    }

    return same_metadata_identity(
        left.metadata(),
        right.metadata()
    );
}
