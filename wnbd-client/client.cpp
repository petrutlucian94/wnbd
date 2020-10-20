#include "client.h"
#include "cmd.h"
#include "usage.h"
#include "wnbd.h"
#include "version.h"

#include <boost/exception/diagnostic_information.hpp>

#include <iostream>
#include <iomanip>

namespace po = boost::program_options;
using namespace std;

vector<Client::Command*> Client::commands;

Client::Command* Client::get_command(string name) {
    for (Client::Command *command : Client::commands) {
        if (command->name == name)
            return command;
        for (auto alias: command->aliases) {
            if (alias == name)
                return command;
        }
    }

    return nullptr;
}

void Client::get_common_options(po::options_description *options) {
    options->add_options()
        ("debug", po::bool_switch(), "Enable debug logging.");
}

void handle_common_options(po::variables_map &vm) {
}

DWORD Client::execute(int argc, const char** argv) {
    vector<string> args;
    args.insert(args.end(), argv + 1, argv + argc);

    // The first must argument must be the command name.
    if (args.size() == 0) {
        args.push_back("help");
    }
    string command_name = args[0];

    Client::Command* command = get_command(command_name);
    if (!command) {
        cerr << "Unknown command: " << command_name << endl << endl
             << "See wnbd-client --help." << endl;
        return ERROR_INVALID_PARAMETER;
    }
    // Remove the command from the list of arguments.
    args.erase(args.begin());

    try {
        po::positional_options_description positional_opts;
        po::options_description named_opts;

        if (command->get_options) {
            command->get_options(&positional_opts, &named_opts);
        }
        get_common_options(&named_opts);

        po::variables_map vm;
        po::store(po::command_line_parser(args)
            .options(named_opts)
            .positional(positional_opts)
            .run(), vm);

        handle_common_options(vm);
        return command->execute(vm);
    }
    // TODO: see if we really need to handle all those exceptions separately.
    catch (po::required_option& e) {
        cerr << "wnbd-client: " << e.what() << endl;
        return ERROR_INVALID_PARAMETER;
    }
    catch (po::too_many_positional_options_error&) {
        cerr << "wnbd-client: too many arguments" << endl;
        return ERROR_INVALID_PARAMETER;
    }
    catch (po::error& e) {
        cerr << "wnbd-client: " << e.what() << endl;
        return ERROR_INVALID_PARAMETER;
    }
    catch (...) {
        cerr << "wnbd-client: Caught unexpected exception." << endl << endl;
        cerr << boost::current_exception_diagnostic_information() << endl;
        return ERROR_INTERNAL_ERROR;
    }
}

DWORD execute_version(const po::variables_map &vm)
{
    return CmdVersion();
}

DWORD execute_help(const po::variables_map &vm)
{
    if (vm.count("command-name")) {
        string command_name = vm["command-name"].as<string>();
        return print_command_help(command_name);
    }
    print_commands();
    return 0;
}

void get_help_args(
    po::positional_options_description *positonal_opts,
    po::options_description *named_opts)
{
    positonal_opts->add("command-name", 1);
    named_opts->add_options()
        ("command-name", po::value<string>(), "Command name.");
}

void get_test_args(
    po::positional_options_description *positonal_opts,
    po::options_description *named_opts)
{
    positonal_opts->add("command-name", 1);
    named_opts->add_options()
        ("command-name", po::value<string>(), "Command name.")
        ("some-arg", po::value<string>(), "some-arg.")
        ("str", po::value<string>(), "str")
        ("dword", po::value<DWORD>()->default_value(10), "")
        ("int", po::value<int>(), "")
        ("uint64", po::value<UINT64>(), "")
        ("bool", po::bool_switch(), "");
}

template <class T>
T safe_get_param(const po::variables_map& vm, string name, T default_val) {
    if (vm.count(name)) {
        return vm[name].as<T>();
    }
    return default_val;
}

DWORD execute_test(const po::variables_map& vm) {
    // string str = vm[""];
    // DWORD dword;
    // int Int;
    // UINT64 uint64;
    // BOOLEAN b;
    cout << "str" << " " << safe_get_param<string>(vm, "str", "") << " "
         << "dword" << " " << safe_get_param<DWORD>(vm, "dword", 0) << " "
        << "int" << " " << safe_get_param<int>(vm, "int", -1) << " "
        << "uint64" << " " << safe_get_param<UINT64>(vm, "uint64", 0) << " "
        << "bool" << " " << safe_get_param<bool>(vm, "bool", 0) << " "
        << endl;
    return 0;
}

Client::Command commands[] = {
    Client::Command(
        "version", {"-v"}, "Get the client, lib and driver version.",
        execute_version),
    Client::Command(
        "help", {"-h", "--help"},
        "List all commands or get more details about a specific command.",
        execute_help, get_help_args),
    Client::Command(
        "test", {"-t"},
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa "
        "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz zzzzzzzzz ccccc ddddddddddddddddddddddddddddddddd ee\n"
        "abc def\nghi jkl",
        execute_test, get_test_args),
    // Client::Command(
    //     "list", {"ls"}, "List WNBD disks",
    //     execute_list),
    // Client::Command(
    //     "map", {}, "Create new disk mapping.",
    //     execute_map, get_map_args),
    // Client::Command(
    //     "unmap", {"rm"}, "Remove disk mapping.",
    //     execute_unmap, get_unmap_args),
    // Client::Command(
    //     "stats", {}, "Get disk stats.",
    //     execute_stats, get_stats_args),
    // Client::Command(
    //     "list-opt", {}, "List driver options.",
    //     execute_list_opt),
    // Client::Command(
    //     "get-opt", {}, "Get driver option.",
    //     execute_get_opt, get_opt_getter_args),
    // Client::Command(
    //     "set-opt", {}, "Set driver option.",
    //     execute_set_opt, get_opt_setter_args),
    // Client::Command(
    //     "reset-opt", {}, "Reset driver option.",
    //     execute_reset_opt, get_reset_opt_args),
};
