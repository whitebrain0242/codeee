#include "chat_server.hpp"
#include "database_config.hpp"
#include "mysql_friend_repository.hpp"
#include "mysql_user_repository.hpp"
#include "protocol.hpp"

#include <iostream>
#include <string>

//主入口函数：
/*1. 解析配置
  2. 初始化依赖
  3. 启动服务
*/

int main(int argc, char *argv[])
{//参数解析与默认值设置
    int port = 9000;
    std::string config_path="config/mysql.conf";
    if (argc >= 2 && !chat::parse_port(argv[1], port))
    {
        std::cerr << "invalid port; expected 1-65535\n";
        return 1;
    }

    //加载数据库配置：
    /*1.创建一个配置对象
      2.加载配置
      3.加载环境变量
      4.检验内容是否正确比如表是否存在,数据库名称是否合法
    */
    if(argc>=3){
        config_path=argv[2];
    }
    chat::DatabaseConfig database_config;
    std::string error;

    if(!chat::load_database_config(config_path,database_config,error)){
        std::cerr
            << "database configuration error: "
            << error
            << std::endl;
        return 1;
    }
    chat::apply_database_environment_overrides(database_config);

    if(!chat::is_valid_database_identifier(database_config.database)){
        std::cerr
            << "database configuration error: "
            << "invalid database name"
            << std::endl;
        return 1;
    }

    //初始化用户仓库
    /*1. 创建用户仓库对象
      2. 调用initialize初始化
      3. run,进入主事件循环，接受处理连接
    */
   

    chat::MysqlUserRepository user_repository(database_config);
    chat::MySqlFriendRepository friend_repository(database_config);
   if(!user_repository.initialize(error)){
    std::cerr
            << "MySQL initialization failed: "
            << error
            << std::endl;
        return 1;
   }

   chat::ChatServer server(port,user_repository,friend_repository);
   if(!server.initialize())return 1;
   return server.run();

}
