#include "prx/libSceAgcDriver/Graphics/include/PerformanceControls.hpp"
#include <iostream>
#include <stdexcept>
#include <string>

int main() {
    using AgcDriver::Graphics::PerformanceControls;
    const auto require = [](bool condition) { if (!condition) throw std::runtime_error("performance controls check failed"); };
    try {
        PerformanceControls controls(0);
        require(controls.Apply("merge_draw_barriers=1\nbda_table_device_local=1\n") && controls.Get() == 3);
        require(controls.Apply("bda_table_device_local=0\r\nmerge_draw_barriers=1\r\n") && controls.Get() == 1);
        for (const auto malformed : {"", "merge_draw_barriers=0", "merge_draw_barriers=0\nbda_table_device_local=",
            "merge_draw_barriers=0\nbda_table_device_local=2", "merge_draw_barriers=0\nbda_table_device_local=0\nextra=1",
            "merge_draw_barriers=0\nmerge_draw_barriers=1\nbda_table_device_local=0"}) {
            require(!controls.Apply(malformed) && controls.Get() == 1);
        }
        require(!controls.Apply(std::string(257, '\n')) && controls.Get() == 1);
        require(controls.Apply("merge_draw_barriers=0\nbda_table_device_local=0\n") && controls.Get() == 0);
        std::cout << "Live controls: complete updates and malformed/partial edit rejection passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
