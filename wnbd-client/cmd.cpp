#include "cmd.h"
#include "wnbd.h"
#include "version.h"

#include <iostream>
#include <iomanip>

#include <boost/algorithm/string/join.hpp>

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

void get_common_options(po::options_description *named_opts) {
    // TODO: use verbose or debug instead
    named_opts->add_options()
        ("log-level", po::value<int>()->default_value(WnbdLogLevelWarning));
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

        // TODO: handle exceptions.
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
    catch (po::too_many_positional_options_error& e) {
        cerr << "wnbd-client: too many arguments" << endl;
        return ERROR_INVALID_PARAMETER;
    }
    catch (po::error& e) {
        cerr << "wnbd-client: " << e.what() << endl;
        return ERROR_INVALID_PARAMETER;
    }
}

DWORD execute_version(const po::variables_map &vm)
{
    cout << "wnbd-client.exe: " << WNBD_VERSION_STR << endl;

    WNBD_VERSION Version = { 0 };

    WnbdGetLibVersion(&Version);
    cout << "libwnbd.dll: " << Version.Description << endl;

    Version = { 0 };
    DWORD Status = WnbdGetDriverVersion(&Version);
    if (!Status) {
        cout << "wnbd.sys: " << Version.Description << endl;
    }

    return Status;
}

DWORD print_command_help(string command_name)
{
    auto command = Client::get_command(command_name);
    if (!command) {
        cerr << "Unknown command: " << command << endl;
        return ERROR_INVALID_PARAMETER;
    }

    po::positional_options_description positional_opts;
    po::options_description named_opts;

    if (command->get_options) {
        command->get_options(&positional_opts, &named_opts);
    }
    // TODO: fix format
    cout << named_opts << endl;

    return 0;
}

DWORD execute_help(const po::variables_map &vm)
{
    if (vm.count("command-name")) {
        string command_name = vm["command-name"].as<string>();
        return print_command_help(command_name);
    }

    cout << "wnbd-client commands: " << endl << endl;

    size_t max_width = 0;
    for (Client::Command *command : Client::commands)
    {
        size_t width = command->name.length();
        if (!command->aliases.empty()) {
            for (auto alias: command->aliases) {
                width += alias.length();
            }
        }
        width += (command->aliases.size() * 3);
        max_width = max(max_width, width);
    }
    for (Client::Command *command : Client::commands)
    {
        vector<string> names;
        names.push_back(command->name);
        names.insert(names.end(), command->aliases.begin(),
                     command->aliases.end());
        string joined_cmd_names = boost::algorithm::join(names, " | ");
        // TODO: limit row length
        cout << left << setw(max_width) << joined_cmd_names
             << " " << command->description << endl;
    }

    // TODO: consider printing common options
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

Client::Command commands[] = {
    Client::Command(
        "version", {"-v"}, "Get the client, lib and driver version.",
        execute_version),
    Client::Command(
        "help", {"-h", "--help"},
        "List all commands or get more details about a specific command.",
        execute_help, get_help_args),
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
