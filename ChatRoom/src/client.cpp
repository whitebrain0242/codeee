#include "client/client_app.hpp"

#include "protocol.hpp"

#include <iostream>

int main(int argc, char *argv[]) {
  ClientAppConfig config;
  // 如果传入了参数值`那么就直接覆盖掉默认值
  if (argc >= 2) {
    config.host = argv[1];
  }

  if (argc >= 3 && !parse_port(argv[2], config.port)) {
    std::cerr << "端口号无效\n";
    return 1;
  }

  if (argc >= 4) {
    config.sqlite_path = argv[3];
  }

  if (argc >= 5) {
    config.download_root = argv[4];
  }

  if (argc >= 6) {
    config.tls_config_path = argv[5];
  }

  ClientApp app;

  return app.run(config);
}
