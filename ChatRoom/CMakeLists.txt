#生命此项目的最低camke版本是3.16,如果低于此版本，会报错提示升级
cmake_minimum_required(VERSION 3.16)
##定义项目的名字是chatroom_v0，会用于一些默认变量，比如说${PROJECT_NAME}，会在ide中显示项目名字
#没有制定名字，默认是c或者c++这里只编译c++,所以没有问题
project(chatroom_v1)
#设置c++标注是c++17,如果版本更低，那么配置失败
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
#定义两个可执行文件的目标：
#chatserver由源文件server.cpp构建，下一个类似
#cmake会自动生成编译和链接规则，生成对应的可执行文件
add_executable(chat_server server.cpp)
add_executable(chat_client client.cpp)
#这个文件的作用是什么？
#使用mkdir build && cd build
 #  cmake ..
 #  make
#就会编译出两个可执行文件，分别对应服务端和客户端，是一个最小可行构建配置