#include "sixseven/cli/pg_wire_codec.h"
#include "sixseven/cli/repl.h"
#include "sixseven/cli/result_formatter.h"
#include "sixseven/cli/socket_client.h"
#include "sixseven/common/platform.h"

#include <iostream>
#include <optional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Argument parsing
// ---------------------------------------------------------------------------

struct CliArgs {
    std::string host{"localhost"};
    uint16_t port{6767};
    std::string user{"sixseven"};
    std::string database{"sixseven"};
    std::string one_shot; // -c "SQL"
    bool help{false};
};

static void print_usage(const char* argv0) {
    std::cerr << "Usage: " << argv0 << " [-h host] [-p port] [-U user] [-d database] [-c \"SQL\"]\n"
              << "\n"
              << "Options:\n"
              << "  -h host      Server host (default: localhost)\n"
              << "  -p port      Server port (default: 6767)\n"
              << "  -U user      Database user (default: sixseven)\n"
              << "  -d database  Database name (default: sixseven)\n"
              << "  -c SQL       Execute SQL and exit (non-interactive)\n"
              << "  --help       Show this help\n"
              << "\n"
              << "In the REPL, terminate statements with ; and use \\q to quit.\n";
}

static bool parse_args(int argc, char* argv[], CliArgs& args) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-?") {
            args.help = true;
            return true;
        }
        if (arg == "-h" && i + 1 < argc) {
            args.host = argv[++i];
        } else if (arg == "-p" && i + 1 < argc) {
            try {
                int p = std::stoi(argv[++i]);
                if (p < 1 || p > 65535) {
                    std::cerr << "Invalid port: " << argv[i] << "\n";
                    return false;
                }
                args.port = static_cast<uint16_t>(p);
            } catch (...) {
                std::cerr << "Invalid port: " << argv[i] << "\n";
                return false;
            }
        } else if (arg == "-U" && i + 1 < argc) {
            args.user = argv[++i];
        } else if (arg == "-d" && i + 1 < argc) {
            args.database = argv[++i];
        } else if (arg == "-c" && i + 1 < argc) {
            args.one_shot = argv[++i];
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Query execution (uses SocketClient)
// ---------------------------------------------------------------------------

static sixseven::Result<void>
execute_sql(sixseven::cli::SocketClient& client, const std::string& sql, std::ostream& out) {
    auto qmsg = sixseven::cli::encode_query_message(sql);
    auto send_result = client.send_bytes(qmsg);
    if (!send_result) {
        return sixseven::make_error(send_result.error().code, send_result.error().message);
    }

    std::vector<std::string> col_names;
    std::vector<std::vector<std::optional<std::string>>> rows;
    std::string command_tag;

    while (true) {
        auto msg = client.read_message();
        if (!msg) {
            return sixseven::make_error(msg.error().code, msg.error().message);
        }

        switch (msg->tag) {
        case sixseven::cli::ServerMsgTag::RowDescription:
            col_names.clear();
            rows.clear();
            for (const auto& col : msg->row_desc.columns) {
                col_names.push_back(col.name);
            }
            break;

        case sixseven::cli::ServerMsgTag::DataRow: {
            std::vector<std::optional<std::string>> row;
            row.reserve(msg->data_row.fields.size());
            for (const auto& f : msg->data_row.fields) {
                row.push_back(f);
            }
            rows.push_back(std::move(row));
            break;
        }

        case sixseven::cli::ServerMsgTag::CommandComplete:
            command_tag = msg->cmd_complete.tag;
            break;

        case sixseven::cli::ServerMsgTag::ErrorResponse:
            out << "ERROR: ";
            if (!msg->error_resp.sql_state.empty()) {
                out << msg->error_resp.sql_state << ": ";
            }
            out << msg->error_resp.message << "\n";
            if (!msg->error_resp.detail.empty()) {
                out << "DETAIL: " << msg->error_resp.detail << "\n";
            }
            if (!msg->error_resp.hint.empty()) {
                out << "HINT: " << msg->error_resp.hint << "\n";
            }
            break;

        case sixseven::cli::ServerMsgTag::NoticeResponse:
            out << "NOTICE: " << msg->notice.message << "\n";
            break;

        case sixseven::cli::ServerMsgTag::ReadyForQuery:
            if (!col_names.empty() || !command_tag.empty()) {
                out << sixseven::cli::format_result_table(col_names, rows, command_tag);
            }
            return sixseven::ok();

        default:
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    CliArgs args;
    if (!parse_args(argc, argv, args)) {
        print_usage(argv[0]);
        return 1;
    }
    if (args.help) {
        print_usage(argv[0]);
        return 0;
    }

    if (!sixseven::platform_init()) {
        std::cerr << "Failed to initialize networking\n";
        return 1;
    }

    sixseven::cli::SocketClient client;
    auto conn_result = client.connect(args.host, args.port);
    if (!conn_result) {
        std::cerr << "Connection failed: " << conn_result.error().message << "\n";
        sixseven::platform_cleanup();
        return 1;
    }

    auto startup_result = client.startup(args.user, args.database);
    if (!startup_result) {
        std::cerr << "Startup failed: " << startup_result.error().message << "\n";
        sixseven::platform_cleanup();
        return 1;
    }

    std::cerr << "Connected to SixSevenDB at " << args.host << ":" << args.port
              << " (user=" << args.user << ", db=" << args.database << ")\n";

    sixseven::cli::ReplOptions repl_opts;
    repl_opts.one_shot = args.one_shot;
    repl_opts.interactive = args.one_shot.empty();

    auto exec_fn = [&](const std::string& sql) -> sixseven::Result<void> {
        return execute_sql(client, sql, std::cout);
    };

    auto repl_result = sixseven::cli::run_repl(std::cin, std::cout, exec_fn, repl_opts);

    client.disconnect();
    sixseven::platform_cleanup();

    if (!repl_result) {
        std::cerr << "Session ended with error: " << repl_result.error().message << "\n";
        return 1;
    }
    return 0;
}
