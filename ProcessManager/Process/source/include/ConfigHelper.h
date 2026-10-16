#pragma once 

#include <legacy/common/conf/IConfigurationManager.h>  
#include <legacy/common/logger/Log.h>                   
#include <string_view>
#include <vector>
#include <string>
namespace process_manager
{
	class ConfigHelper
	{
	public:
		static void Initialize(legacy::conf::IConfigurationManager* conf);

		template <typename T, typename T1, typename T2, typename T3>
		static void ReadValue(const char* sectionDesc, const char* valueDesc, T& value, T1 defaultValue, T2 minValue, T3 maxValue)
		{
			if (ConfigHelper::ReadConfiguration(sectionDesc, valueDesc, value))
			{
				ConfigHelper::Limit(value, static_cast<T>(minValue), static_cast<T>(maxValue));
			}
			else
			{
				value = static_cast<T>(defaultValue);
			}
		}

		static legacy::enums::RetVal ReadConfiguration(const char* sectionDesc, const char* valueDesc, const char*& value, const char* defaultValue)
		{
			value = GetObjectRef().conf->GetValue(sectionDesc, valueDesc);
			if (value)
			{
				return legacy::enums::RetVal::Success;
			}
			else
			{
				value = defaultValue;
				return legacy::enums::RetVal::NullPointer;
			}
		}

		static inline std::string_view TrimSv(std::string_view s)
		{
			while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
			while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))  s.remove_suffix(1);
			return s;
		}

		// Input: "py1: cpp1:py2::  :"
		// Output: {"py1","cpp1","py2"}
		static inline void SplitElementsToVector(const char* depends_cstr, char delimiter,
			std::vector<std::string>& out)
		{
			out.clear();
			if (!depends_cstr || !*depends_cstr) return;

			std::string_view s(depends_cstr);

			while (true)
			{
				const size_t pos = s.find(delimiter);
				std::string_view token = (pos == std::string_view::npos) ? s : s.substr(0, pos);
				token = TrimSv(token);

				if (!token.empty()) out.emplace_back(token);

				if (pos == std::string_view::npos) break;
				s.remove_prefix(pos + 1);
			}
		}

	private:


		static legacy::enums::RetVal ReadConfiguration(const char* sectionDesc, const char* valueDesc, int32_t& value)
		{
			auto ret = GetObjectRef().conf->GetValueInt(sectionDesc, valueDesc, &value);
			return ret;
		}
		static legacy::enums::RetVal ReadConfiguration(const char* sectionDesc, const char* valueDesc, uint32_t& value)
		{
			auto ret = GetObjectRef().conf->GetValueUInt(sectionDesc, valueDesc, &value);
			return ret;
		}
		static legacy::enums::RetVal ReadConfiguration(const char* sectionDesc, const char* valueDesc, double& value)
		{
			auto ret = GetObjectRef().conf->GetValueDouble(sectionDesc, valueDesc, &value);
			return ret;
		}
		static legacy::enums::RetVal ReadConfiguration(const char* sectionDesc, const char* valueDesc, float& value)
		{
			double readValue = 0.0; // left unwritten when the key is missing
			auto ret = GetObjectRef().conf->GetValueDouble(sectionDesc, valueDesc, &readValue);
			value = static_cast<float>(readValue);
			return ret;
		}
		static legacy::enums::RetVal ReadConfiguration(const char* sectionDesc, const char* valueDesc, bool& value)
		{
			bool readValue = false; // left unwritten when the key is missing
			auto ret = GetObjectRef().conf->GetValueBool(sectionDesc, valueDesc, &readValue);
			value = static_cast<bool>(readValue);
			return ret;
		}
		static legacy::enums::RetVal ReadConfiguration(const char* sectionDesc, const char* valueDesc, uint16_t& value)
		{
			uint32_t readValue = 0; // left unwritten when the key is missing
			auto ret = GetObjectRef().conf->GetValueUInt(sectionDesc, valueDesc, &readValue);
			value = static_cast<uint16_t>(readValue);
			return ret;
		}

		template <typename T>
		static void Limit(T& value, T minValue, T maxValue)
		{
			if (value < minValue)
			{
				value = minValue;
			}
			else if (value > maxValue)
			{
				value = maxValue;
			}
		}

		static ConfigHelper& GetObjectRef();

		ConfigHelper() = default;
		~ConfigHelper() = default;

		legacy::conf::IConfigurationManager* conf;
	};

}   // namespace process_manager