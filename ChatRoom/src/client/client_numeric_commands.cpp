#include "client/client_numeric_commands.hpp"
#include "protocol.hpp"
#include <charconv>
#include <iostream>

bool parse_numeric_command_line(const std::string &line,
                                ParsedNumericCommand &command) {
  const std::string cleaned = trim(line);
  if (cleaned.empty()) return false;

  const std::size_t separator = cleaned.find_first_of(" \t");
  const std::string number_text =
      separator == std::string::npos ? cleaned : cleaned.substr(0, separator);

  int number = 0;
  const char *begin = number_text.data();
  const char *end = begin + number_text.size();
  const auto result = std::from_chars(begin, end, number);

  if (result.ec != std::errc() || result.ptr != end ||
      !is_known_numeric_command(number)) {
    return false;
  }

  command.number = number;
  command.arguments =
      separator == std::string::npos ? std::string()
                                     : trim(cleaned.substr(separator + 1U));
  return true;
}

const char *numeric_command_name(int number) {
  switch (number) {
  case 1: return "REGISTER";
  case 2: return "LOGIN";
  case 3: return "LOGOUT";
  case 4: return "DELETE_ACCOUNT";
  case 5: return "SAY";
  case 6: return "MSG";
  case 7: return "GROUP_MSG";
  case 8: return "ENTER_PRIVATE";
  case 9: return "ENTER_GROUP";
  case 10: return "LEAVE_CHAT";
  case 11: return "HISTORY_PRIVATE";
  case 12: return "HISTORY_GROUP";
  case 13: return "FILES";
  case 14: return "SEND_FILE";
  case 15: return "FRIENDS";
  case 16: return "FRIEND_REQUESTS";
  case 17: return "ADD_FRIEND";
  case 18: return "ACCEPT_FRIEND";
  case 19: return "REJECT_FRIEND";
  case 20: return "REMOVE_FRIEND";
  case 21: return "BLOCK_FRIEND";
  case 22: return "UNBLOCK_FRIEND";
  case 23: return "BLOCKED_FRIENDS";
  case 24: return "CREATE_GROUP";
  case 25: return "DISSOLVE_GROUP";
  case 26: return "APPLY_GROUP";
  case 27: return "MY_GROUPS";
  case 28: return "LEAVE_GROUP";
  case 29: return "GROUP_MEMBERS";
  case 30: return "ADD_GROUP_ADMIN";
  case 31: return "REMOVE_GROUP_ADMIN";
  case 33: return "APPROVE_GROUP";
  case 34: return "REJECT_GROUP";
  case 35: return "REMOVE_GROUP_MEMBER";
  case 36: return "WHO";
  case 37: return "PENDING";
  case 38: return "HELP";
  case 39: return "LOCAL_HELP";
  case 40: return "LOCAL_DB";
  case 41: return "HISTORY_PUBLIC";
  default: return nullptr;
  }
}

bool is_known_numeric_command(int number) {
  return numeric_command_name(number) != nullptr;
}

void print_numeric_help() {
  std::cout
      << "[本地] 完整数字命令帮助：\n"
      << "  1  <用户名> <密码>                  注册账户\n"
      << "  2  <用户名> <密码>                  登录账户\n"
      << "  3                                   退出登录\n"
      << "  4  <密码> CONFIRM                   注销账户\n"
      << "  5  <消息>                            发送公共消息\n"
      << "  6                                   当前会话发送消息并选择输入模式\n"
      << "  8  <用户名>                          进入好友私聊\n"
      << "  9  <群名称>                          进入群聊\n"
      << " 10                                   退出当前会话\n"
      << " 11  <用户名> [条数]                   查看本地私聊历史（1-200）\n"
      << " 12  <群名称> [条数]                   查看本地群聊历史（1-200）\n"
      << " 13                                   查看当前会话文件记录\n"
      << " 14  <文件路径>                        向当前会话发送文件\n"
      << " 15                                   好友列表\n"
      << " 16                                   好友申请\n"
      << " 17  <用户名>                          添加好友\n"
      << " 18  <用户名>                          通过好友申请\n"
      << " 19  <用户名>                          拒绝好友申请\n"
      << " 20  <用户名>                          删除好友\n"
      << " 21  <用户名>                          屏蔽好友\n"
      << " 22  <用户名>                          解除屏蔽\n"
      << " 23                                   屏蔽列表\n"
      << " 24  <群名称>                          创建群\n"
      << " 25  <群名称>                          解散群\n"
      << " 26  <群名称>                          申请加入群\n"
      << " 27                                   我的群\n"
      << " 28  <群名称>                          退出群\n"
      << " 29  <群名称>                          查看群成员\n"
      << " 30  <群名称> <用户名>                 设置群管理员\n"
      << " 31  <群名称> <用户名>                 取消群管理员\n"
      << " 33                                   自动列出全部可处理申请并通过\n"
      << " 34                                   自动列出全部可处理申请并拒绝\n"
      << " 35  <群名称> <用户名>                 移出群成员\n"
      << " 36                                   在线用户\n"
      << " 37                                   待处理离线消息/文件\n"
      << " 38                                   查看服务端帮助\n"
      << " 39                                   查看本帮助\n"
      << " 40                                   查看本地 SQLite/下载路径\n"
      << " 41  [条数]                            公共消息历史\n"
      << "[本地] 进入好友或群会话后，必须先输入 6，再选择：\n"
      << "       1=回车立即发送；2=长文本编辑模式（支持换行）。\n"
      << "[本地] 数字 32 已取消用户入口；33/34 会自动调用公共申请查询流程。\n";
}
