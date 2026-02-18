#include <coretypes/stringobject_factory.h>
#include <opendaq/module_manager_factory.h>
#include <testutils/daq_memcheck_listener.h>
#include <testutils/testutils.h>

int main(int argc, char** args)
{
    {
        daq::ModuleManager(".");
    }
    testing::InitGoogleTest(&argc, args);

    testing::TestEventListeners& listeners = testing::UnitTest::GetInstance()->listeners();
    listeners.Append(new DaqMemCheckListener());

    auto res = RUN_ALL_TESTS();

    return res;
}
