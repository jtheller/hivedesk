#include "hivedesk/shell.h"
#include "input_state.h"
#include <mutex>
#include <stdexcept>

namespace hive {namespace {
std::mutex injected_mutex;
InjectedInputState injected;
bool send(INPUT& input){return SendInput(1,&input,sizeof(input))==1;}
}
void release_injected_input(){
    const std::lock_guard lock(injected_mutex);
    // Failed releases remain tracked for a retry on the accessible desktop.
    injected.release(send);
}
void inject_input(const InputEvent& event){
    const std::lock_guard lock(injected_mutex);
    SetLastError(ERROR_SUCCESS);
    if(!injected.apply(event,send))throw std::runtime_error("Input injection denied: "+std::to_string(GetLastError()));
}
}
