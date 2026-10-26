#include "hivedesk/transport.h"
#include <iostream>
int main() {
    std::string result;
    bool ok = hive::Transport::self_test(result);
    std::cout << result << '\n';
    return ok ? 0 : 1;
}
