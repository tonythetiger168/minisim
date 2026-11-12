#include "expr.hpp"
#include <iostream>
#include <string>

Val digit(int n) {
    return n == 0 ? mk(0) : n == 1 ? mk(1) : n == 2 ? allx(1) : allz(1);
}
Val pair(int n) { return v_concat({digit(n / 4), digit(n % 4)}); }
std::string bits(Val value) {
    std::string text;
    for (int i = value.w - 1; i >= 0; --i)
        text += (value.z >> i & 1) ? 'z' : (value.x >> i & 1) ? 'x' :
                (value.v >> i & 1) ? '1' : '0';
    return text;
}
int main(int argc, char** argv) {
    bool dump = argc == 2 && std::string(argv[1]) == "--dump";
    int failures = 0;
    const char states[] = "01xz";
    for (int c = 0; c < 4; ++c)
        for (int a = 0; a < 16; ++a)
            for (int b = 0; b < 16; ++b) {
                auto left = bits(pair(a)), right = bits(pair(b));
                std::string expected = c == 0 ? right : left;
                if (c >= 2)
                    for (int i = 0; i < 2; ++i)
                        expected[i] = left[i] == right[i] ? left[i] : 'x';
                auto result = bits(v_mux(digit(c), pair(a), pair(b)));
                if (dump) std::cout << result << '\n';
                if (result != expected) {
                    if (failures < 5)
                        std::cerr << states[c] << " ? " << left << " : " << right
                                  << " -> " << result << "; expected " << expected << '\n';
                    ++failures;
                }
            }
    // Exercise the top bit and ensure the result representation remains canonical.
    auto wide = v_mux(allx(1), allz(64), allz(64));
    if (wide.w != 64 || wide.z != ~0ULL || wide.x != ~0ULL || wide.v != 0)
        ++failures;
    if (failures) std::cerr << failures << " mux cases failed\n";
    return failures ? 1 : 0;
}
