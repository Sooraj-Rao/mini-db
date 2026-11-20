#include "storage.hpp"

#include <exception>
#include <iostream>
#include <sstream>
#include <string>

namespace {
void print_help() {
    std::cout
        << "Commands:\n"
        << "  SET <key> <value> [EX seconds]  Store a value, optionally with TTL\n"
        << "  GET <key>                       Read a value\n"
        << "  EXPIRE <key> <seconds>          Set expiration on an existing key\n"
        << "  TTL <key>                       Show remaining seconds (-1 no TTL, -2 missing)\n"
        << "  PERSIST <key>                   Remove expiration\n"
        << "  DELETE <key>                    Delete a key\n"
        << "  SIZE               Show number of stored keys\n"
        << "  STATS              Show database file size and live key count\n"
        << "  COMPACT            Remove obsolete records from the data file\n"
        << "  HELP               Show this help\n"
        << "  EXIT               Close MiniDB\n\n"
        << "Values may contain spaces. Keys cannot contain spaces.\n";
}
}

int main() {
    try {
        minidb::Storage db("data/minidb.db");
        std::cout << "MiniDB v0.8.0\n"
                  << "Loaded " << db.size() << " key(s) from data/minidb.db\n"
                  << "Type HELP for commands.\n\n";

        std::string line;
        while (true) {
            std::cout << "minidb> ";
            if (!std::getline(std::cin, line)) break;

            std::istringstream input(line);
            std::string command;
            input >> command;
            if (command.empty()) continue;

            if (command == "EXIT" || command == "exit" ||
                command == "QUIT" || command == "quit") {
                std::cout << "Bye.\n";
                break;
            }
            if (command == "HELP" || command == "help") {
                print_help();
                continue;
            }
            if (command == "SIZE" || command == "size") {
                std::cout << db.size() << " key(s)\n";
                continue;
            }
            if (command == "STATS" || command == "stats") {
                std::cout << "Live keys: " << db.size() << '\n'
                          << "Database file: " << db.file_size() << " bytes\n";
                continue;
            }
            if (command == "COMPACT" || command == "compact") {
                const auto before = db.file_size();
                db.compact();
                const auto after = db.file_size();
                std::cout << "Compaction complete: " << before << " -> "
                          << after << " bytes (saved "
                          << (before >= after ? before - after : 0)
                          << " bytes)\n";
                continue;
            }
            if (command == "SET" || command == "set") {
                std::string key;
                input >> key;
                std::string value;
                std::getline(input >> std::ws, value);
                if (key.empty() || value.empty()) {
                    std::cout << "Usage: SET <key> <value>\n";
                    continue;
                }
                const auto marker = value.rfind(" EX ");
                if (marker != std::string::npos) {
                    const auto seconds_text = value.substr(marker + 4);
                    try { std::size_t used=0; auto seconds=std::stoull(seconds_text,&used); if(used!=seconds_text.size() || !seconds) throw std::invalid_argument("invalid seconds"); value.resize(marker); if(value.empty()) throw std::invalid_argument("value cannot be empty"); db.set_with_ttl(key,value,seconds); }
                    catch (const std::exception& e) { std::cout << "Error: " << e.what() << '\n'; continue; }
                } else db.set(key, value);
                std::cout << "OK\n";
                continue;
            }
            if (command == "GET" || command == "get") {
                std::string key;
                input >> key;
                if (key.empty()) {
                    std::cout << "Usage: GET <key>\n";
                    continue;
                }
                const auto value = db.get(key);
                if (value) std::cout << *value << '\n';
                else std::cout << "(not found)\n";
                continue;
            }
            if (command == "EXPIRE" || command == "expire") {
                std::string key, seconds_text; input >> key >> seconds_text;
                try { std::size_t used=0; auto seconds=std::stoull(seconds_text,&used); if(key.empty() || used!=seconds_text.size() || !seconds) throw std::invalid_argument("usage: EXPIRE <key> <seconds>"); std::cout << (db.expire(key,seconds) ? "1\n" : "0\n"); }
                catch (const std::exception& e) { std::cout << "Error: " << e.what() << '\n'; }
                continue;
            }
            if (command == "TTL" || command == "ttl") { std::string key; input >> key; if(key.empty()) std::cout << "Usage: TTL <key>\n"; else std::cout << db.ttl(key) << "\n"; continue; }
            if (command == "PERSIST" || command == "persist") { std::string key; input >> key; if(key.empty()) std::cout << "Usage: PERSIST <key>\n"; else std::cout << (db.persist(key) ? "1\n" : "0\n"); continue; }
            if (command == "DELETE" || command == "delete") {
                std::string key;
                input >> key;
                if (key.empty()) {
                    std::cout << "Usage: DELETE <key>\n";
                    continue;
                }
                if (db.remove(key)) std::cout << "OK\n";
                else std::cout << "(not found)\n";
                continue;
            }
            std::cout << "Unknown command. Type HELP.\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MiniDB error: " << error.what() << '\n';
        return 1;
    }
}
