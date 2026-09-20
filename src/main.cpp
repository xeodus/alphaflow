#include <print>
#include <vector>

int main() {
    std::println("Hello World!");
    std::vector<int> v;
    v.push_back(2);
    std::println("vector: {}", v.back());
}
