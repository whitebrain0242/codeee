#pragma once

#include "client/client_state.hpp"

#include <string>

class SqliteClient;
class TlsClientTransport;
//从本地Sqlite中加载上次没有完成的上传任务,向服务器发送续传请求,尝试恢复终端的上传
bool load_and_resume_pending_uploads(
    TlsClientTransport& transport,//TLS网络传输层
    ClientState& state,//当前客户端的运行时状态
    SqliteClient& cache//本地Sqlite缓存,查询之前的元数据(文件路径,以上传偏移量,Token)
);
//准备上传前的准备动作
bool prepare_upload(
    TlsClientTransport& transport,
    ClientState& state,
    SqliteClient& cache,//Sqlite
    const std::string& scope,//上传范围
    const std::string& target,//接收方
    const std::string& raw_path//在本地磁盘的文件路径
);
//当客户端收到服务器发来的以 FILE_ 开头的文本行,解析行,调用对应的处理逻辑
bool handle_file_protocol_line(
    TlsClientTransport& transport,
    const std::string& line,//服务器受到的原始文本行
    ClientState& state,
    SqliteClient& cache
);
//客户端主动断开连接的时候,推出程序或者取消下载调用,遍历state.downloads，对于未完成的下载，可能会将临时文件（.part）保留在磁盘上
void preserve_partial_downloads(
    ClientState& state
);
