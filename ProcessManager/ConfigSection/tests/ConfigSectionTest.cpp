#include <gtest/gtest.h>

#include "ConfigSection.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{

config_section::ConfigValue Parse(const std::string& text)
{
    config_section::ConfigValue document{};
    std::string error;
    EXPECT_EQ(config_section::ParseConfigText(text, document, error), config_section::LoadCode::Ok) << error;
    return document;
}

bool HasProblemWith(const config_section::ConfigSection& section, const std::string& part)
{
    for (const std::string& problem : section.Problems())
    {
        if (problem.find(part) != std::string::npos)
        {
            return true;
        }
    }

    return false;
}

} // namespace

TEST(ConfigSectionTest, ParsesServicesList)
{
    const config_section::ConfigValue document =
        Parse("Services:\n  - ProcessName: A\n    Args: [--port, 1001]\n  - ProcessName: B\n");
    const config_section::ConfigValue* services = config_section::FindValue(document, "Services");
    ASSERT_NE(services, nullptr);
    ASSERT_EQ(services->kind, config_section::ValueKind::Sequence);
    ASSERT_EQ(services->children.size(), 2U);
    const config_section::ConfigValue* name = config_section::FindValue(services->children[1], "ProcessName");
    ASSERT_NE(name, nullptr);
    EXPECT_EQ(name->text, "B");
}

TEST(ConfigSectionTest, SyntaxErrorNamesLineAndColumnFromOne)
{
    config_section::ConfigValue document{};
    std::string error;
    EXPECT_EQ(config_section::ParseConfigText("a:\n  b: 1\n c: 2\n", document, error), config_section::LoadCode::SyntaxError);
    EXPECT_EQ(error.rfind("line 3, column 2: ", 0), 0U) << error;
}

TEST(ConfigSectionTest, DoubleSpaceInPlainValueNamesTheFix)
{
    config_section::ConfigValue document{};
    std::string error;
    EXPECT_EQ(config_section::ParseConfigText("Mode: Auto\nArgs: --flag  value\n", document, error),
              config_section::LoadCode::SyntaxError);
    EXPECT_NE(error.find("Line 2 has two spaces in a row in a value without quotes"), std::string::npos) << error;
    EXPECT_EQ(config_section::ParseConfigText("Mode: Auto\nArgs: '--flag  value'\n", document, error),
              config_section::LoadCode::Ok)
        << error;
}

TEST(ConfigSectionTest, DuplicateKeyIsSyntaxError)
{
    config_section::ConfigValue document{};
    std::string error;
    EXPECT_EQ(config_section::ParseConfigText("a: 1\na: 2\n", document, error), config_section::LoadCode::SyntaxError);
    EXPECT_NE(error.find("line 2"), std::string::npos) << error;
}

TEST(ConfigSectionTest, EmptyTextIsNull)
{
    EXPECT_EQ(Parse("# only a comment\n").kind, config_section::ValueKind::Null);
}

TEST(ConfigSectionTest, MissingFileReturnsFileNotOpened)
{
    config_section::ConfigValue document{};
    std::string error;
    EXPECT_EQ(config_section::LoadConfigFile("/no/such/config.yaml", document, error),
              config_section::LoadCode::FileNotOpened);
    EXPECT_FALSE(error.empty());
}

TEST(ConfigSectionTest, LoadsFile)
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "config_section_test.yaml";
    {
        std::ofstream file{path};
        file << "ProcessManager:\n  Mode: Auto\n";
    }

    config_section::ConfigValue document{};
    std::string error;
    EXPECT_EQ(config_section::LoadConfigFile(path, document, error), config_section::LoadCode::Ok) << error;
    EXPECT_NE(config_section::FindValue(document, "ProcessManager"), nullptr);
    std::filesystem::remove(path);
}

TEST(ConfigSectionTest, ReadStringGivesNumbersInResolvedForm)
{
    const config_section::ConfigValue document = Parse("hex: 0x10\nfraction: 0.1\ncolon: :1\n");
    config_section::ConfigSection section{document, "test"};
    EXPECT_EQ(section.ReadString("hex", ""), "16");
    EXPECT_EQ(section.ReadString("fraction", ""), "0.1");
    EXPECT_EQ(section.ReadString("colon", ""), ":1");
    EXPECT_EQ(section.ReadString("missing", "fallback"), "fallback");
}

TEST(ConfigSectionTest, ReadBoolAcceptsWordsAndDigits)
{
    const config_section::ConfigValue document = Parse("a: yes\nb: On\nc: 0\nd: True\ne: \"false\"\n");
    config_section::ConfigSection section{document, "test"};
    EXPECT_TRUE(section.ReadBool("a", false));
    EXPECT_TRUE(section.ReadBool("b", false));
    EXPECT_FALSE(section.ReadBool("c", true));
    EXPECT_TRUE(section.ReadBool("d", false));
    EXPECT_FALSE(section.ReadBool("e", true));
    EXPECT_TRUE(section.Problems().empty());
}

TEST(ConfigSectionTest, ReadBoolReportsOtherText)
{
    const config_section::ConfigValue document = Parse("CheckHealth: maybe\n");
    config_section::ConfigSection section{document, "service 'A'"};
    EXPECT_TRUE(section.ReadBool("CheckHealth", true));
    EXPECT_TRUE(HasProblemWith(section, "service 'A': CheckHealth: 'maybe' is not true or false"));
}

TEST(ConfigSectionTest, ReadIntReadsQuotedNumbers)
{
    const config_section::ConfigValue document = Parse("a: 42\nb: \"1000\"\nc: +7\n");
    config_section::ConfigSection section{document, "test"};
    EXPECT_EQ(section.ReadInt("a", 0, 0, 100), 42);
    EXPECT_EQ(section.ReadInt("b", 0, 0, 5000), 1000);
    EXPECT_EQ(section.ReadInt("c", 0, 0, 100), 7);
    EXPECT_TRUE(section.Problems().empty());
}

TEST(ConfigSectionTest, ReadIntKeepsRangeAndReports)
{
    const config_section::ConfigValue document = Parse("low: 500\nhigh: 10000000000000000000\n");
    config_section::ConfigSection section{document, "test"};
    EXPECT_EQ(section.ReadInt("low", 10000, 1000, 2000000), 1000);
    EXPECT_EQ(section.ReadInt("high", 1, 0, 1000), 1000);
    EXPECT_TRUE(HasProblemWith(section, "low: 500 is below the minimum 1000"));
    EXPECT_TRUE(HasProblemWith(section, "is above the maximum 1000"));
}

TEST(ConfigSectionTest, ReadIntRejectsText)
{
    const config_section::ConfigValue document = Parse("MaxRestarts: many\nhalf: 1.5\n");
    config_section::ConfigSection section{document, "test"};
    EXPECT_EQ(section.ReadInt("MaxRestarts", 5, 0, 1000), 5);
    EXPECT_EQ(section.ReadInt("half", 3, 0, 10), 3);
    EXPECT_TRUE(HasProblemWith(section, "MaxRestarts: 'many' is not a whole number; 5 is used"));
}

TEST(ConfigSectionTest, ReadDoubleKeepsRange)
{
    const config_section::ConfigValue document = Parse("a: 2.5\nb: 20\nc: nope\n");
    config_section::ConfigSection section{document, "test"};
    EXPECT_DOUBLE_EQ(section.ReadDouble("a", 2.0, 1.0, 10.0), 2.5);
    EXPECT_DOUBLE_EQ(section.ReadDouble("b", 2.0, 1.0, 10.0), 10.0);
    EXPECT_DOUBLE_EQ(section.ReadDouble("c", 2.0, 1.0, 10.0), 2.0);
    EXPECT_TRUE(HasProblemWith(section, "c: 'nope' is not a number"));
}

TEST(ConfigSectionTest, ReadChoiceIgnoresCase)
{
    const config_section::ConfigValue document = Parse("RestartMode: always\nStdioMode: Sometimes\n");
    config_section::ConfigSection section{document, "test"};
    EXPECT_EQ(section.ReadChoice("RestartMode", {"Never", "OnFailure", "Always"}, 1), 2);
    EXPECT_EQ(section.ReadChoice("StdioMode", {"Inherit", "Pipe"}, 0), 0);
    EXPECT_TRUE(HasProblemWith(section, "'Sometimes' is not one of Inherit, Pipe; Inherit is used"));
}

TEST(ConfigSectionTest, ReadListFromSequenceOrText)
{
    const config_section::ConfigValue document = Parse("a: [--port, 1001, 'two words']\nb: --flag value\n");
    config_section::ConfigSection section{document, "test"};
    EXPECT_EQ(section.ReadList("a", ' '), (std::vector<std::string>{"--port", "1001", "two words"}));
    EXPECT_EQ(section.ReadList("b", ' '), (std::vector<std::string>{"--flag", "value"}));
    EXPECT_TRUE(section.ReadList("missing", ' ').empty());
}

TEST(ConfigSectionTest, ReadAssignmentsFromMappingSequenceOrText)
{
    const config_section::ConfigValue document =
        Parse("map:\n  PYTHONPATH: /usr/lib\n  EMPTY:\nlist: [A=1, B=2]\ntext: A=1;B=2\n");
    config_section::ConfigSection section{document, "test"};
    EXPECT_EQ(section.ReadAssignments("map", ';'), (std::vector<std::string>{"EMPTY=", "PYTHONPATH=/usr/lib"}));
    EXPECT_EQ(section.ReadAssignments("list", ';'), (std::vector<std::string>{"A=1", "B=2"}));
    EXPECT_EQ(section.ReadAssignments("text", ';'), (std::vector<std::string>{"A=1", "B=2"}));
}

TEST(ConfigSectionTest, UnreadKeyIsUnknown)
{
    const config_section::ConfigValue document = Parse("ProcessName: A\nForceKilTimeoutMs: 100\n");
    config_section::ConfigSection section{document, "service 'A'"};
    EXPECT_EQ(section.ReadString("ProcessName", ""), "A");
    EXPECT_TRUE(section.Has("ForceKilTimeoutMs"));
    EXPECT_TRUE(HasProblemWith(section, "service 'A': unknown key 'ForceKilTimeoutMs'"));
}

TEST(ConfigSectionTest, NullValueCountsAsMissing)
{
    const config_section::ConfigValue document = Parse("Cwd:\nArgs: ~\n");
    config_section::ConfigSection section{document, "test"};
    EXPECT_FALSE(section.Has("Cwd"));
    EXPECT_EQ(section.ReadString("Cwd", "/tmp"), "/tmp");
    EXPECT_TRUE(section.ReadList("Args", ' ').empty());
    EXPECT_TRUE(section.Problems().empty());
}

TEST(ConfigSectionTest, ListForSingleValueIsProblem)
{
    const config_section::ConfigValue document = Parse("ProcessName: [A, B]\n");
    config_section::ConfigSection section{document, "test"};
    EXPECT_EQ(section.ReadString("ProcessName", "none"), "none");
    EXPECT_TRUE(HasProblemWith(section, "ProcessName: must be a single value"));
}

TEST(ConfigSectionTest, NonMappingSectionIsProblem)
{
    const config_section::ConfigValue document = Parse("- just\n- a list\n");
    const config_section::ConfigSection section{document, "the configuration"};
    EXPECT_TRUE(HasProblemWith(section, "the configuration: must be a mapping"));
}

TEST(ConfigSectionTest, ReadSectionAndSequenceCheckKinds)
{
    const config_section::ConfigValue document = Parse("ProcessManager: Auto\nServices:\n  - ProcessName: A\n");
    config_section::ConfigSection section{document, "the configuration"};
    EXPECT_EQ(section.ReadSection("ProcessManager"), nullptr);
    EXPECT_NE(section.ReadSequence("Services"), nullptr);
    EXPECT_TRUE(HasProblemWith(section, "ProcessManager: must be a mapping"));
}

TEST(ConfigSectionTest, SplitListDropsEmptyParts)
{
    EXPECT_EQ(config_section::SplitList(" a  b ;", ' '), (std::vector<std::string>{"a", "b", ";"}));
    EXPECT_EQ(config_section::SplitList("A=1;;B=2 ", ';'), (std::vector<std::string>{"A=1", "B=2"}));
    EXPECT_TRUE(config_section::SplitList("", ' ').empty());
}
