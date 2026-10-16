#include <ConfigHelper.h>

namespace process_manager
{
    ConfigHelper& ConfigHelper::GetObjectRef()
    {
        static ConfigHelper conf;
        return conf;
    }

    void ConfigHelper::Initialize(legacy::conf::IConfigurationManager* conf)
    {
        GetObjectRef().conf = conf;
    }

}   // namespace sms