#include "giodb/common/logging.h"

int main() {
    giodb::init_logging("info");
    GIODB_LOG_INFO("GioDB Server v0.1.0 starting");
    return 0;
}
