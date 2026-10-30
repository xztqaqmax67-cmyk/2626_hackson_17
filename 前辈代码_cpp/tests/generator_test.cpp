#include "generator.hpp"
#include <atomic>
using namespace problem2;
void require(bool value) {
    if (!value) {
        throw std::runtime_error("generator protocol check failed");
    }
}
int main() {
    std::atomic<int> steps{0}, destroyed{0};
    struct Guard {
        std::atomic<int> &count;
        ~Guard() {
            ++count;
        }
    };
    execution_deadline = monotonic() + 120;
    const auto first_deadline = execution_deadline;
    {
        Generator<int> source([&](const auto &yield) {
            Guard guard{destroyed};
            require(execution_deadline == first_deadline);
            ++steps;
            yield(7);
            require(execution_deadline == first_deadline + 1);
            ++steps;
            execution_deadline = first_deadline + 2;
            yield(9);
            ++steps; // 销毁已暂停的生成器不应执行到这里。
        });
        require(steps == 0);
        require(source.next() == 7 && steps == 1);
        auto moved = std::move(source);
        require(!source.next());
        execution_deadline = first_deadline + 1;
        require(moved.next() == 9 && steps == 2);
        require(execution_deadline == first_deadline + 2);
    }
    require(destroyed == 1 && steps == 2);
    Generator<int> failing([](const auto &yield) {
        yield(1);
        throw std::invalid_argument("propagated");
    });
    require(failing.next() == 1);
    bool caught = false;
    try {
        failing.next();
    } catch (const std::invalid_argument &) {
        caught = true;
    }
    require(caught && !failing.next());
    Generator<int> empty([](const auto &) {});
    require(!empty.next() && !empty.next());
    execution_deadline = 0;
    std::cout << "PASS lazy resume / cancellation / move / exception / deadline transfer\n";
}
