#include <iostream>
#include <optional>
#include <string>

#include "../include/DB.h"

template <typename T>
void print_optional(const std::optional<T>& value) {
    if (value) {
        std::cout << *value << '\n';
    } else {
        std::cout << "nullopt\n";
    }
}

int main() {
    std::cout << "starting main" << std::endl;
    DB db("test.db");

    db.put("a", "123");
    db.put("b", "456");

    print_optional(db.get("a"));  // 123
    print_optional(db.get("b"));  // 456

    db.put("a", "999");
    print_optional(db.get("a"));  // 999

    db.remove("b");
    print_optional(db.get("b"));  // nullopt

    return 0;
}