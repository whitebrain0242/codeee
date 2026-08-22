#include "config.hpp"
#include "protocol.hpp"

#include <charconv>
#include <fstream>
#include <unordered_map>

namespace {
// 字符串转无符号整数（端口
bool parse_unsigned(const std::string &text, unsigned int &value) {
  unsigned int parsed = 0;
  const char *begin = text.data();
  const char *end = begin + text.size();
  const auto result = std::from_chars(begin, end, parsed);

  if (result.ec != std::errc() || result.ptr != end) {
    return false;
  }
  value = parsed;
  return true;
}
// 字符串转布尔值
bool parse_bool(const std::string &text, bool &value) {
  if (text == "true" || text == "1" || text == "yes" || text == "on") {
    value = true;
    return true;
  }
  if (text == "false" || text == "0" || text == "no" || text == "off") {
    value = false;
    return true;
  }
  return false;
}
// 对文件的处理(读配置文件+键值对格式)保存在values中
bool load_key_values(const std::string &path, const std::string &label,
                     std::unordered_map<std::string, std::string> &values,
                     std::string &error) {
  std::ifstream input(path);
  if (!input) {
    error = "cannot open " + label + " config: " + path;
    return false;
  }

  std::string line;
  std::size_t line_number = 0;

  while (std::getline(input, line)) {
    ++line_number;
    const std::string cleaned = trim(line); // 去掉首尾空格

    // 忽略空行 和 以 # 或 ; 开头的注释行
    if (cleaned.empty() || cleaned[0] == '#' || cleaned[0] == ';') {
      continue;
    }

    // 查找第一个等号
    const std::size_t equals = cleaned.find('=');
    if (equals == std::string::npos) {
      error =
          "invalid " + label + " config line " + std::to_string(line_number);
      return false;
    }

    // 截取 key 和 value 并分别 trim
    const std::string key = trim(cleaned.substr(0, equals));
    const std::string value = trim(cleaned.substr(equals + 1));

    if (key.empty()) {
      error = "empty config key on line " + std::to_string(line_number);
      return false;
    }

    values[key] = value;
  }
  return true;
}

} // namespace
// 加载数据库配置：存储聊天记录
bool load_mysql_config(const std::string &path, MySqlConfig &config,
                       std::string &error) {
  std::unordered_map<std::string, std::string> values;
  if (!load_key_values(path, "MySQL", values, error))
    return false;

  // 字符串字段：直接覆盖（如果配置里有）
  if (values.count("host"))
    config.host = values["host"];
  if (values.count("user"))
    config.user = values["user"];
  if (values.count("password"))
    config.password = values["password"];
  if (values.count("database"))
    config.database = values["database"];

  // 端口校验（1 ~ 65535）
  if (values.count("port")) {
    unsigned int parsed = 0;
    if (!parse_unsigned(values["port"], parsed) || parsed == 0U ||
        parsed > 65535U) {
      error = "invalid MySQL port";
      return false;
    }
    config.port = parsed;
  }

  // 连接超时校验（1 ~ 60 秒）
  if (values.count("connect_timeout_seconds")) {
    unsigned int parsed = 0;
    if (!parse_unsigned(values["connect_timeout_seconds"], parsed) ||
        parsed == 0U || parsed > 60U) {
      error = "invalid connect_timeout_seconds";
      return false;
    }
    config.connect_timeout_seconds = parsed;
  }

  if (values.count("pool_size")) {
    unsigned int parsed = 0;
    if (!parse_unsigned(values["pool_size"], parsed) || parsed == 0U ||
        parsed > 64U) {
      error = "MySQL pool_size must be 1-64";
      return false;
    }
    config.pool_size = parsed;
  }

  // 强制检查：user 和 database 必填
  if (config.user.empty()) {
    error = "MySQL config requires user";
    return false;
  }
  if (config.database.empty()) {
    error = "MySQL config requires database";
    return false;
  }
  return true;
}

// 管理在线状态
bool load_redis_config(const std::string &path, RedisConfig &config,
                       std::string &error) {
  std::unordered_map<std::string, std::string> values;

  if (!load_key_values(path, "Redis", values, error)) {
    return false;
  }

  if (values.count("host")) {
    config.host = values["host"];
  }
  if (values.count("password")) {
    config.password = values["password"];
  }
  if (values.count("key_prefix")) {
    config.key_prefix = values["key_prefix"];
  }
  if (values.count("server_name")) {
    config.server_name = values["server_name"];
  }

  if (values.count("port")) {
    unsigned int parsed = 0;
    if (!parse_unsigned(values["port"], parsed) || parsed == 0U ||
        parsed > 65535U) {
      error = "invalid Redis port";
      return false;
    }
    config.port = parsed;
  }

  // 特殊校验1：Redis 数据库编号只能 0~255
  if (values.count("database")) {
    unsigned int parsed = 0;
    if (!parse_unsigned(values["database"], parsed) || parsed > 255U) {
      error = "invalid Redis database";
      return false;
    }
    config.database = parsed;
  }

  // 特殊校验2：连接超时最多 30000 毫秒（30秒）
  if (values.count("connect_timeout_ms")) {
    unsigned int parsed = 0;
    if (!parse_unsigned(values["connect_timeout_ms"], parsed) || parsed == 0U ||
        parsed > 30000U) {
      error = "invalid Redis connect_timeout_ms";
      return false;
    }
    config.connect_timeout_ms = parsed;
  }

  // 特殊校验3：在线状态 TTL 必须 30~86400 秒（1分钟 ~ 24小时）
  if (values.count("presence_ttl_seconds")) {
    unsigned int parsed = 0;
    if (!parse_unsigned(values["presence_ttl_seconds"], parsed) ||
        parsed < 30U || parsed > 86400U) {
      error = "presence_ttl_seconds must be 30-86400";
      return false;
    }
    config.presence_ttl_seconds = parsed;
  }

  // 强制必填项：host, key_prefix, server_name
  if (config.host.empty()) {
    error = "Redis config requires host";
    return false;
  }
  if (config.key_prefix.empty()) {
    error = "Redis config requires key_prefix";
    return false;
  }
  if (config.server_name.empty()) {
    error = "Redis config requires server_name";
    return false;
  }
  return true;
}
// 服务端证书
bool load_tls_server_config(const std::string &path, TlsServerConfig &config,
                            std::string &error) {
  std::unordered_map<std::string, std::string> values;

  if (!load_key_values(path, "TLS server", values, error)) {
    return false;
  }

  if (values.count("enabled")) {
    if (!parse_bool(values["enabled"], config.enabled)) {
      error = "invalid TLS server enabled value";
      return false;
    }
  }

  if (values.count("certificate_file"))
    config.certificate_file = values["certificate_file"];
  if (values.count("private_key_file"))
    config.private_key_file = values["private_key_file"];

  // 强制开启TLS
  if (!config.enabled) {
    error = "TLS server enabled=false is not allowed in chatroom v8.4";
    return false;
  }

  // 证书和私钥文件路径必须存在（路径为空时报错）
  if (config.certificate_file.empty() || config.private_key_file.empty()) {
    error = "TLS server config requires certificate_file and private_key_file";
    return false;
  }
  return true;
}
// 客户端验证服务端证书,保存到config中
bool load_tls_client_config(const std::string &path, TlsClientConfig &config,
                            std::string &error) {
  std::unordered_map<std::string, std::string> values;

  if (!load_key_values(path, "TLS client", values, error)) {
    return false;
  }
  //有没有开启
  if (values.count("enabled")) {
    if (!parse_bool(values["enabled"], config.enabled)) {
      error = "invalid TLS client enabled value";
      return false;
    }
  }
  //是否验证证书
  if (values.count("verify_peer")) {
    if (!parse_bool(values["verify_peer"], config.verify_peer)) {
      error = "invalid TLS client verify_peer value";
      return false;
    }
  }
  //读取路径配置
  if (values.count("ca_file"))
    config.ca_file = values["ca_file"];//CA证书路径
  if (values.count("server_name"))
    config.server_name = values["server_name"];//服务端域名

  // 同样强制开启 TLS,但是身份验证不是这个
  if (!config.enabled) {
    error = "TLS client enabled=false is not allowed in chatroom v8.4";
    return false;
  }

  // 如果要求验证对端（verify_peer=true），CA 证书必填
  if (config.verify_peer && config.ca_file.empty()) {
    error = "TLS client verify_peer=true requires ca_file";
    return false;
  }
  return true;
}
