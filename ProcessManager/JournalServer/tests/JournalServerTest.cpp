#include <gtest/gtest.h>

#include "JournalServer.hpp"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
#include <zmq.hpp>
#include <zmq_addon.hpp>

namespace
{

constexpr std::chrono::milliseconds reply_wait{10000};
const std::string unit_parent = "/system.slice/processmanager.service";
const journal_server::ManagerIdentity unit_identity = journal_server::IdentityFromCgroup(unit_parent, 4242);
const std::string fake_members = "cgroup\tGGM\n"
                                 "pid\tGGM\t10\t2097152\tpython3\tpython3 /opt/main.py\n"
                                 "pid\tGGM\t20\t0\t\t\n"
                                 "cgroup\tYAM\n";

std::vector<std::string> Tail(std::vector<std::string> args)
{
    const std::vector<std::string> tail{"--no-pager", "--all", "--quiet", "-o", "json"};
    args.insert(args.end(), tail.begin(), tail.end());
    return args;
}

journal_server::JournalRequest Request(journal_server::TargetKind kind, std::string value)
{
    journal_server::JournalRequest request{};
    request.kind = kind;
    request.value = std::move(value);
    return request;
}

// A folder of stand-in programs, removed with the test.
class ScriptFolder
{
public:
    ScriptFolder()
    {
        char pattern[] = "/tmp/journal_server_test_XXXXXX";
        path = ::mkdtemp(pattern);
    }

    ~ScriptFolder()
    {
        std::error_code ignored{};
        std::filesystem::remove_all(path, ignored);
    }

    ScriptFolder(const ScriptFolder&) = delete;
    ScriptFolder& operator=(const ScriptFolder&) = delete;

    std::string Add(const std::string& name, const std::string& body) const
    {
        const std::filesystem::path script = path / name;
        std::ofstream{script} << "#!/bin/sh\n" << body;
        std::filesystem::permissions(script, std::filesystem::perms::owner_all);
        return script.string();
    }

    void Write(const std::string& name, const std::string& content) const
    {
        const std::filesystem::path file = path / name;
        std::filesystem::create_directories(file.parent_path());
        std::ofstream{file, std::ios::binary} << content;
    }

    std::string Read(const std::string& name) const
    {
        std::ifstream file{path / name};
        return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    }

    bool Has(const std::string& name) const
    {
        return std::filesystem::exists(path / name);
    }

    std::filesystem::path path;
};

// A REQ client of a server under test.
class Client
{
public:
    explicit Client(const std::string& endpoint)
    {
        socket.set(zmq::sockopt::linger, 0);
        socket.set(zmq::sockopt::rcvtimeo, static_cast<int>(reply_wait.count()));
        socket.connect(endpoint);
    }

    std::vector<std::string> Ask(const std::vector<std::string>& frames)
    {
        std::vector<zmq::const_buffer> buffers{};
        for (const std::string& frame : frames)
        {
            buffers.push_back(zmq::buffer(frame));
        }

        zmq::send_multipart(socket, buffers);
        std::vector<zmq::message_t> reply{};
        if (!zmq::recv_multipart(socket, std::back_inserter(reply)).has_value())
        {
            return {"no reply"};
        }

        std::vector<std::string> texts{};
        for (const zmq::message_t& frame : reply)
        {
            texts.push_back(frame.to_string());
        }

        return texts;
    }

    zmq::context_t context{1};
    zmq::socket_t socket{context, zmq::socket_type::req};
};

journal_server::JournalServer Started(const std::string& journalctl, const std::string& systemctl,
                                      journal_server::ManagerIdentity identity = unit_identity,
                                      journal_server::SystemPaths paths = {})
{
    journal_server::JournalServer server{"tcp://127.0.0.1:*", std::move(identity), journalctl, systemctl,
                                         std::move(paths)};
    std::string error{};
    EXPECT_EQ(server.Start(error), journal_server::StartCode::Ok) << error;
    return server;
}

// The cgroups and /proc of a manager in processmanager.service, inside the test's folder: GGM runs pid 10 and
// pid 20, which has gone; YAM's cgroup is empty.
journal_server::SystemPaths FakeSystem(const ScriptFolder& folder, const std::string& parent = unit_parent)
{
    folder.Write("cgroup" + parent + "/task_pm/cgroup.procs", "4242\n");
    folder.Write("cgroup" + parent + "/task_GGM/cgroup.procs", "20\n10\n");
    folder.Write("cgroup" + parent + "/task_YAM/cgroup.procs", "");
    folder.Write("cgroup" + parent + "/other/cgroup.procs", "30\n");
    folder.Write("cgroup" + parent + "/task_file", "");
    folder.Write("proc/10/comm", "python3\n");
    folder.Write("proc/10/status", "Name:\tpython3\nVmRSS:\t    2048 kB\nVmSwap:\t       0 kB\n");
    folder.Write("proc/10/cmdline", std::string{"python3"} + '\0' + "/opt/main.py" + '\0');
    journal_server::SystemPaths paths{};
    paths.cgroupRoot = folder.path / "cgroup";
    paths.procRoot = folder.path / "proc";
    return paths;
}

std::vector<std::string> Lines(const std::string& text)
{
    std::vector<std::string> lines{};
    std::istringstream stream{text};
    for (std::string line{}; std::getline(stream, line);)
    {
        lines.push_back(line);
    }

    return lines;
}

} // namespace

TEST(JournalServerTest, IdentityFindsUnitInCgroup)
{
    EXPECT_EQ(unit_identity.unit, "processmanager.service");
    EXPECT_EQ(unit_identity.serviceCgroupParent, "/system.slice/processmanager.service");
    EXPECT_EQ(unit_identity.pid, 4242);
}

TEST(JournalServerTest, IdentityTakesInnermostService)
{
    const journal_server::ManagerIdentity identity =
        journal_server::IdentityFromCgroup("/user.slice/user@1000.service/app.slice/pm.service", 7);
    EXPECT_EQ(identity.unit, "pm.service");
}

TEST(JournalServerTest, IdentityOutsideUnitHasNone)
{
    const journal_server::ManagerIdentity identity = journal_server::IdentityFromCgroup("/", 7);
    EXPECT_EQ(identity.unit, "");
    EXPECT_EQ(identity.serviceCgroupParent, "/");
}

TEST(JournalServerTest, IdentityNormalizesSlashes)
{
    const journal_server::ManagerIdentity identity = journal_server::IdentityFromCgroup("system.slice//a.service/", 7);
    EXPECT_EQ(identity.serviceCgroupParent, "/system.slice/a.service");
    EXPECT_EQ(identity.unit, "a.service");
}

TEST(JournalServerTest, ParseReadsEveryFrame)
{
    journal_server::JournalRequest request{};
    ASSERT_EQ(journal_server::ParseJournalRequest({"journal", "service", "GGM", "20", "s=1"}, request),
              journal_server::RequestCode::Ok);
    EXPECT_EQ(request.kind, journal_server::TargetKind::Service);
    EXPECT_EQ(request.value, "GGM");
    EXPECT_EQ(request.lines, 20);
    EXPECT_EQ(request.afterCursor, "s=1");
}

TEST(JournalServerTest, ParseFillsDefaults)
{
    journal_server::JournalRequest request{};
    ASSERT_EQ(journal_server::ParseJournalRequest({"journal", "manager"}, request), journal_server::RequestCode::Ok);
    EXPECT_EQ(request.kind, journal_server::TargetKind::Manager);
    EXPECT_EQ(request.lines, journal_server::default_lines);
    EXPECT_EQ(request.afterCursor, "");
}

TEST(JournalServerTest, ParseRejectsUnknownKind)
{
    journal_server::JournalRequest request{};
    EXPECT_EQ(journal_server::ParseJournalRequest({"journal", "unit", "x"}, request),
              journal_server::RequestCode::UnknownKind);
}

TEST(JournalServerTest, ParseRejectsLineCountWithLetters)
{
    journal_server::JournalRequest request{};
    EXPECT_EQ(journal_server::ParseJournalRequest({"journal", "manager", "", "5x"}, request),
              journal_server::RequestCode::BadLines);
}

TEST(JournalServerTest, ParseRejectsOtherRequest)
{
    journal_server::JournalRequest request{};
    EXPECT_EQ(journal_server::ParseJournalRequest({"status"}, request), journal_server::RequestCode::UnknownRequest);
}

TEST(JournalServerTest, ManagerJournalUsesUnit)
{
    std::vector<std::string> args{};
    ASSERT_EQ(journal_server::BuildJournalArgs(Request(journal_server::TargetKind::Manager, ""), unit_identity, args),
              journal_server::RequestCode::Ok);
    EXPECT_EQ(args, Tail({"-u", "processmanager.service", "-n", "50"}));
}

TEST(JournalServerTest, ManagerJournalWithoutUnitUsesPid)
{
    std::vector<std::string> args{};
    ASSERT_EQ(journal_server::BuildJournalArgs(Request(journal_server::TargetKind::Manager, ""),
                                               journal_server::IdentityFromCgroup("/", 99), args),
              journal_server::RequestCode::Ok);
    EXPECT_EQ(args, Tail({"_PID=99", "-n", "50"}));
}

TEST(JournalServerTest, ServiceJournalUsesTaskCgroup)
{
    std::vector<std::string> args{};
    ASSERT_EQ(journal_server::BuildJournalArgs(Request(journal_server::TargetKind::Service, "GGM"), unit_identity, args),
              journal_server::RequestCode::Ok);
    EXPECT_EQ(args, Tail({"_SYSTEMD_CGROUP=/system.slice/processmanager.service/task_GGM", "-n", "50"}));
}

TEST(JournalServerTest, ServiceJournalAtCgroupRoot)
{
    std::vector<std::string> args{};
    ASSERT_EQ(journal_server::BuildJournalArgs(Request(journal_server::TargetKind::Service, "GGM"),
                                               journal_server::IdentityFromCgroup("/", 1), args),
              journal_server::RequestCode::Ok);
    EXPECT_EQ(args.front(), "_SYSTEMD_CGROUP=/task_GGM");
}

TEST(JournalServerTest, BadServiceNamesAreRefused)
{
    const std::vector<std::string> names{"", "..", ".", "a/b", "line\nbreak", std::string(64, 'x')};
    for (const std::string& name : names)
    {
        std::vector<std::string> args{};
        EXPECT_EQ(journal_server::BuildJournalArgs(Request(journal_server::TargetKind::Service, name), unit_identity, args),
                  journal_server::RequestCode::BadService)
            << name;
    }
}

TEST(JournalServerTest, BadPidsAreRefused)
{
    const std::vector<std::string> pids{"", "0", "-1", "12a", "9999999999", " 12"};
    for (const std::string& pid : pids)
    {
        std::vector<std::string> args{};
        EXPECT_EQ(journal_server::BuildJournalArgs(Request(journal_server::TargetKind::Pid, pid), unit_identity, args),
                  journal_server::RequestCode::BadPid)
            << pid;
    }
}

TEST(JournalServerTest, PidJournalUsesPid)
{
    std::vector<std::string> args{};
    ASSERT_EQ(journal_server::BuildJournalArgs(Request(journal_server::TargetKind::Pid, "1234"), unit_identity, args),
              journal_server::RequestCode::Ok);
    EXPECT_EQ(args, Tail({"_PID=1234", "-n", "50"}));
}

TEST(JournalServerTest, LineCountIsClamped)
{
    journal_server::JournalRequest request = Request(journal_server::TargetKind::Pid, "1");
    std::vector<std::string> args{};
    request.lines = 0;
    ASSERT_EQ(journal_server::BuildJournalArgs(request, unit_identity, args), journal_server::RequestCode::Ok);
    EXPECT_EQ(args[2], "1");
    request.lines = 5000;
    ASSERT_EQ(journal_server::BuildJournalArgs(request, unit_identity, args), journal_server::RequestCode::Ok);
    EXPECT_EQ(args[2], std::to_string(journal_server::max_lines));
}

TEST(JournalServerTest, CursorIsPassedOn)
{
    journal_server::JournalRequest request = Request(journal_server::TargetKind::Pid, "1");
    request.afterCursor = "s=abc;i=1f;b=2;m=3;t=4;x=5";
    std::vector<std::string> args{};
    ASSERT_EQ(journal_server::BuildJournalArgs(request, unit_identity, args), journal_server::RequestCode::Ok);
    EXPECT_EQ(args.back(), "--after-cursor=s=abc;i=1f;b=2;m=3;t=4;x=5");
}

TEST(JournalServerTest, BadCursorsAreRefused)
{
    const std::vector<std::string> cursors{"s=1 i=2", "s=1\n", std::string(journal_server::max_cursor_length + 1, 'c')};
    for (const std::string& cursor : cursors)
    {
        journal_server::JournalRequest request = Request(journal_server::TargetKind::Pid, "1");
        request.afterCursor = cursor;
        std::vector<std::string> args{};
        EXPECT_EQ(journal_server::BuildJournalArgs(request, unit_identity, args), journal_server::RequestCode::BadCursor);
    }
}

TEST(JournalServerTest, UnitStatusArguments)
{
    std::vector<std::string> args{};
    ASSERT_EQ(journal_server::BuildUnitStatusArgs(unit_identity, args), journal_server::RequestCode::Ok);
    EXPECT_EQ(args, (std::vector<std::string>{"status", "processmanager.service", "--no-pager", "-l", "-n", "0"}));
    EXPECT_EQ(journal_server::BuildUnitStatusArgs(journal_server::IdentityFromCgroup("/", 1), args),
              journal_server::RequestCode::NoUnit);
}

TEST(JournalServerTest, EveryCodeIsDescribed)
{
    for (const journal_server::RequestCode code :
         {journal_server::RequestCode::UnknownRequest, journal_server::RequestCode::UnknownKind,
          journal_server::RequestCode::BadService, journal_server::RequestCode::BadPid,
          journal_server::RequestCode::BadLines, journal_server::RequestCode::BadCursor,
          journal_server::RequestCode::NoUnit})
    {
        EXPECT_FALSE(journal_server::Describe(code).empty());
    }
}

TEST(JournalServerTest, ReadsServiceCgroupsAndTheirProcesses)
{
    const ScriptFolder folder{};
    const journal_server::SystemPaths paths = FakeSystem(folder);
    std::vector<journal_server::ServiceCgroup> cgroups{};
    ASSERT_EQ(journal_server::ReadServiceCgroups(paths.cgroupRoot / unit_parent.substr(1), paths.procRoot, cgroups),
              journal_server::MembersCode::Ok);
    ASSERT_EQ(cgroups.size(), 2U);
    EXPECT_EQ(cgroups[0].name, "GGM");
    ASSERT_EQ(cgroups[0].members.size(), 2U);
    EXPECT_EQ(cgroups[0].members[0].pid, 10);
    EXPECT_EQ(cgroups[0].members[0].comm, "python3");
    EXPECT_EQ(cgroups[0].members[0].rssBytes, 2048U * 1024U);
    EXPECT_EQ(cgroups[0].members[0].cmdline, "python3 /opt/main.py");
    EXPECT_EQ(cgroups[1].name, "YAM");
    EXPECT_TRUE(cgroups[1].members.empty());
}

TEST(JournalServerTest, ProcessThatHasGoneKeepsOnlyItsPid)
{
    const ScriptFolder folder{};
    const journal_server::SystemPaths paths = FakeSystem(folder);
    std::vector<journal_server::ServiceCgroup> cgroups{};
    ASSERT_EQ(journal_server::ReadServiceCgroups(paths.cgroupRoot / unit_parent.substr(1), paths.procRoot, cgroups),
              journal_server::MembersCode::Ok);
    ASSERT_FALSE(cgroups.empty());
    ASSERT_EQ(cgroups[0].members.size(), 2U);
    EXPECT_EQ(cgroups[0].members[1].pid, 20);
    EXPECT_EQ(cgroups[0].members[1].comm, "");
    EXPECT_EQ(cgroups[0].members[1].rssBytes, 0U);
    EXPECT_EQ(cgroups[0].members[1].cmdline, "");
}

TEST(JournalServerTest, ReadsThisProcessFromProc)
{
    const ScriptFolder folder{};
    folder.Write("cgroup/task_self/cgroup.procs", std::to_string(::getpid()) + "\n");
    std::vector<journal_server::ServiceCgroup> cgroups{};
    ASSERT_EQ(journal_server::ReadServiceCgroups(folder.path / "cgroup", "/proc", cgroups),
              journal_server::MembersCode::Ok);
    ASSERT_EQ(cgroups.size(), 1U);
    ASSERT_EQ(cgroups[0].members.size(), 1U);
    const journal_server::CgroupMember& self = cgroups[0].members[0];
    // The kernel keeps the first 15 bytes of the program's file name as comm.
    const std::string program = std::filesystem::read_symlink("/proc/self/exe").filename().string();
    EXPECT_EQ(self.comm, program.substr(0, 15));
    EXPECT_GT(self.rssBytes, 0U);
    EXPECT_NE(self.cmdline.find(program), std::string::npos) << self.cmdline;
}

TEST(JournalServerTest, LongCommandLineIsCut)
{
    const ScriptFolder folder{};
    folder.Write("cgroup/task_A/cgroup.procs", "5\n");
    folder.Write("proc/5/cmdline", std::string(journal_server::max_cmdline_bytes + 100, 'a'));
    std::vector<journal_server::ServiceCgroup> cgroups{};
    ASSERT_EQ(journal_server::ReadServiceCgroups(folder.path / "cgroup", folder.path / "proc", cgroups),
              journal_server::MembersCode::Ok);
    ASSERT_EQ(cgroups.size(), 1U);
    ASSERT_EQ(cgroups[0].members.size(), 1U);
    EXPECT_EQ(cgroups[0].members[0].cmdline.size(), journal_server::max_cmdline_bytes);
}

TEST(JournalServerTest, MissingCgroupFolderIsReported)
{
    std::vector<journal_server::ServiceCgroup> cgroups{};
    EXPECT_EQ(journal_server::ReadServiceCgroups("/nonexistent/cgroup", "/proc", cgroups),
              journal_server::MembersCode::NoCgroupFolder);
}

TEST(JournalServerTest, FormatWritesOneLinePerCgroupAndProcess)
{
    journal_server::CgroupMember member{};
    member.pid = 10;
    member.comm = "a\tb";
    member.rssBytes = 4096;
    member.cmdline = "x\ny";
    journal_server::ServiceCgroup running{};
    running.name = "GGM";
    running.members.push_back(member);
    journal_server::ServiceCgroup empty{};
    empty.name = "YAM";
    EXPECT_EQ(journal_server::FormatServiceCgroups({running, empty}),
              "cgroup\tGGM\npid\tGGM\t10\t4096\ta b\tx y\ncgroup\tYAM\n");
}

TEST(JournalServerTest, ServerAnswersVersion)
{
    journal_server::JournalServer server = Started("journalctl", "systemctl");
    Client client{server.BoundEndpoint()};
    EXPECT_EQ(client.Ask({"version"}), (std::vector<std::string>{"ok", std::to_string(journal_server::protocol_version)}));
}

TEST(JournalServerTest, ServerAnswersDealerClient)
{
    journal_server::JournalServer server = Started("journalctl", "systemctl");
    zmq::context_t context{1};
    zmq::socket_t dealer{context, zmq::socket_type::dealer};
    dealer.set(zmq::sockopt::linger, 0);
    dealer.set(zmq::sockopt::rcvtimeo, static_cast<int>(reply_wait.count()));
    dealer.connect(server.BoundEndpoint());
    dealer.send(zmq::str_buffer("version"));
    std::vector<zmq::message_t> reply{};
    ASSERT_TRUE(zmq::recv_multipart(dealer, std::back_inserter(reply)).has_value());
    ASSERT_EQ(reply.size(), 2U);
    EXPECT_EQ(reply[0].to_string(), "ok");
}

TEST(JournalServerTest, ServerRunsJournalctlWithBuiltArguments)
{
    const ScriptFolder folder{};
    const std::string journalctl = folder.Add("journalctl",
        "printf '%s\\n' \"$@\" > \"$(dirname \"$0\")/args.txt\"\n"
        "echo '{\"MESSAGE\":\"one\",\"_PID\":\"10\",\"__CURSOR\":\"c1\"}'\n"
        "echo '{\"MESSAGE\":\"two\",\"_PID\":\"10\",\"__CURSOR\":\"c2\"}'\n");
    journal_server::JournalServer server = Started(journalctl, "systemctl");
    Client client{server.BoundEndpoint()};

    const std::vector<std::string> reply = client.Ask({"journal", "service", "GGM", "20", "c0"});
    ASSERT_EQ(reply.size(), 2U);
    EXPECT_EQ(reply[0], "ok");
    EXPECT_EQ(Lines(reply[1]).size(), 2U);
    EXPECT_EQ(Lines(folder.Read("args.txt")),
              (std::vector<std::string>{"_SYSTEMD_CGROUP=/system.slice/processmanager.service/task_GGM", "-n", "20",
                                        "--no-pager", "--all", "--quiet", "-o", "json", "--after-cursor=c0"}));
}

TEST(JournalServerTest, ServerReturnsJournalctlError)
{
    const ScriptFolder folder{};
    const std::string journalctl = folder.Add("journalctl", "echo 'No journal files were found.' >&2\nexit 1\n");
    journal_server::JournalServer server = Started(journalctl, "systemctl");
    Client client{server.BoundEndpoint()};
    EXPECT_EQ(client.Ask({"journal", "manager"}), (std::vector<std::string>{"error", "No journal files were found."}));
}

TEST(JournalServerTest, ServerRefusesBadRequestWithoutRunning)
{
    const ScriptFolder folder{};
    const std::string journalctl = folder.Add("journalctl", "touch \"$(dirname \"$0\")/ran\"\n");
    journal_server::JournalServer server = Started(journalctl, "systemctl");
    Client client{server.BoundEndpoint()};
    EXPECT_EQ(client.Ask({"journal", "service", "a/b"}), (std::vector<std::string>{"error", "bad service name"}));
    EXPECT_EQ(client.Ask({"tail"})[0], "error");
    EXPECT_FALSE(folder.Has("ran"));
}

TEST(JournalServerTest, ServerReportsMissingProgram)
{
    journal_server::JournalServer server = Started("/nonexistent/journalctl", "systemctl");
    Client client{server.BoundEndpoint()};
    const std::vector<std::string> reply = client.Ask({"journal", "pid", "1"});
    ASSERT_EQ(reply.size(), 2U);
    EXPECT_EQ(reply[0], "error");
    EXPECT_EQ(reply[1].rfind("cannot run /nonexistent/journalctl", 0), 0U) << reply[1];
}

TEST(JournalServerTest, ServerAnswersUnitStatusOfStoppedUnit)
{
    const ScriptFolder folder{};
    const std::string systemctl = folder.Add("systemctl", "echo 'Active: inactive (dead)'\nexit 3\n");
    journal_server::JournalServer server = Started("journalctl", systemctl);
    Client client{server.BoundEndpoint()};
    EXPECT_EQ(client.Ask({"unit-status"}), (std::vector<std::string>{"ok", "Active: inactive (dead)\n"}));
}

TEST(JournalServerTest, ServerRefusesUnitStatusOutsideUnit)
{
    journal_server::JournalServer server = Started("journalctl", "systemctl", journal_server::IdentityFromCgroup("/", 1));
    Client client{server.BoundEndpoint()};
    EXPECT_EQ(client.Ask({"unit-status"}),
              (std::vector<std::string>{"error", journal_server::Describe(journal_server::RequestCode::NoUnit)}));
}

TEST(JournalServerTest, ServerAnswersMembers)
{
    const ScriptFolder folder{};
    journal_server::JournalServer server = Started("journalctl", "systemctl", unit_identity, FakeSystem(folder));
    Client client{server.BoundEndpoint()};
    EXPECT_EQ(client.Ask({"members"}), (std::vector<std::string>{"ok", fake_members}));
}

TEST(JournalServerTest, ServerListsMembersOutsideUnit)
{
    const ScriptFolder folder{};
    journal_server::JournalServer server =
        Started("journalctl", "systemctl", journal_server::IdentityFromCgroup("/", 1), FakeSystem(folder, ""));
    Client client{server.BoundEndpoint()};
    EXPECT_EQ(client.Ask({"members"}), (std::vector<std::string>{"ok", fake_members}));
}

TEST(JournalServerTest, ServerReportsMissingCgroupFolder)
{
    const ScriptFolder folder{};
    journal_server::SystemPaths paths{};
    paths.cgroupRoot = folder.path / "none";
    journal_server::JournalServer server = Started("journalctl", "systemctl", unit_identity, paths);
    Client client{server.BoundEndpoint()};
    const std::vector<std::string> reply = client.Ask({"members"});
    ASSERT_EQ(reply.size(), 2U);
    EXPECT_EQ(reply[0], "error");
    EXPECT_EQ(reply[1].rfind("cannot read the manager's cgroup folder", 0), 0U) << reply[1];
}

TEST(JournalServerTest, ServerCutsOversizedOutputAtLastLine)
{
    const ScriptFolder folder{};
    const std::string journalctl = folder.Add("journalctl", "yes '{\"MESSAGE\":\"0123456789\"}' | head -c 6000000\n");
    journal_server::JournalServer server = Started(journalctl, "systemctl");
    Client client{server.BoundEndpoint()};
    const std::vector<std::string> reply = client.Ask({"journal", "manager"});
    ASSERT_EQ(reply.size(), 2U);
    EXPECT_EQ(reply[0], "ok");
    EXPECT_LE(reply[1].size(), journal_server::max_output_bytes);
    EXPECT_GT(reply[1].size(), journal_server::max_output_bytes / 2);
    EXPECT_EQ(reply[1].back(), '\n');
}

TEST(JournalServerTest, StoppingKillsRunningCommand)
{
    const ScriptFolder folder{};
    const std::string journalctl = folder.Add("journalctl", "echo $$ > \"$(dirname \"$0\")/pid.txt\"\nexec sleep 30\n");
    auto server = std::make_unique<journal_server::JournalServer>(Started(journalctl, "systemctl"));
    Client client{server->BoundEndpoint()};
    zmq::send_multipart(client.socket, std::vector<zmq::const_buffer>{zmq::str_buffer("journal"), zmq::str_buffer("manager")});
    for (int i = 0; i < 100 && !folder.Has("pid.txt"); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }

    ASSERT_TRUE(folder.Has("pid.txt"));
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    const pid_t pid = std::stoi(folder.Read("pid.txt"));
    const auto stopping = std::chrono::steady_clock::now();
    server.reset();
    EXPECT_LT(std::chrono::steady_clock::now() - stopping, std::chrono::seconds{2});
    EXPECT_NE(::kill(pid, 0), 0);
}

TEST(JournalServerTest, BindToAddressNotHereFails)
{
    // 192.0.2.1 is reserved for documentation, so no machine has it. (WSL1 lets a second socket bind a port that is
    // in use, so a port clash would not fail there.)
    journal_server::JournalServer server{"tcp://192.0.2.1:6670", unit_identity, "journalctl", "systemctl"};
    std::string error{};
    EXPECT_EQ(server.Start(error), journal_server::StartCode::BindFailed);
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(server.BoundEndpoint(), "");
}

TEST(JournalServerTest, MovedServerKeepsServing)
{
    journal_server::JournalServer first = Started("journalctl", "systemctl");
    const std::string endpoint = first.BoundEndpoint();
    journal_server::JournalServer second{std::move(first)};
    Client client{endpoint};
    EXPECT_EQ(client.Ask({"version"})[0], "ok");
    EXPECT_EQ(first.BoundEndpoint(), "");
}
