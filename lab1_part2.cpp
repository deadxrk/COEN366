// Several generator threads produce random numbers (0-20) and push them into a
// shared queue. Several comparator threads take numbers out of the queue and
// compare them to 10.
//
// Synchronization:
//   - A mutex protects the shared queue, so only one thread touches it at a time.
//   - A condition variable lets comparators sleep until a number is available.
//   - Checking the queue and removing a number happen inside one locked section,
//     so each number is taken by exactly one comparator (no duplicates, no losses).

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <queue>
#include <random>
#include <stop_token>
#include <syncstream>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

// ---------- Configurations ----------
constexpr int  kNumGenerators  = 2;
constexpr int  kNumComparators = 2;
constexpr auto kRunDuration    = 10s;

// ---------- Shared data ----------
std::queue<int> numbers;          // Shared FIFO queue of generated numbers
std::mutex mtx;                   // Protects 'numbers'
std::condition_variable_any cv;   // Signals comparators that the queue is not empty

// Counters used only to verify correctness at the end.
// They are atomic, so threads can increment them safely without the mutex.
std::atomic<int> total_generated{0};
std::atomic<int> total_processed{0};

// ---------- Generator thread ----------
// Generates a random number every second and pushes it into the shared queue.
// 'stop' is supplied automatically by std::jthread; 'id' labels the output.
void generator(std::stop_token stop, int id) {
    // Each generator owns its random engine, because engines are not thread-safe.
    std::random_device rd;
    std::mt19937 engine{rd()};
    std::uniform_int_distribution<int> dist{0, 20};

    while (!stop.stop_requested()) {
        const int value = dist(engine);   // Local work, no lock needed

        {   // ----- Critical section: add the number to the shared queue -----
            std::lock_guard<std::mutex> lock(mtx);
            numbers.push(value);
            ++total_generated;

            // Printed while holding the lock so this line always appears
            // before the comparator line for the same number.
            std::osyncstream(std::cout) << "Generator-" << id
                                        << " generated: " << value << '\n';
        }   // ----- Lock released here -----

        cv.notify_one();   // Wake one waiting comparator

        std::this_thread::sleep_for(1s);
    }
}

// ---------- Comparator thread ----------
// Waits for a number in the shared queue, removes it, and compares it to 10.
void comparator(std::stop_token stop, int id) {
    while (true) {
        int value;

        {   // ----- Critical section: take exactly one number from the queue -----
            std::unique_lock<std::mutex> lock(mtx);

            // Sleep until the queue has a number, or a stop is requested.
            // Returns false only if a stop was requested AND the queue is empty,
            // so comparators finish any leftover numbers before exiting.
            if (!cv.wait(lock, stop, [] { return !numbers.empty(); })) return;

            value = numbers.front();   // Read and remove in the same locked step,
            numbers.pop();             // so no other comparator can get this number
        }   // ----- Lock released here -----

        ++total_processed;

        // Printing happens outside the lock to keep the critical section short.
        std::osyncstream out(std::cout);
        out << "Comparator-" << id << ": " << value;
        if (value > 10)      out << " is greater than 10\n";
        else if (value < 10) out << " is less than 10\n";
        else                 out << " is equal to 10\n";
    }
}

// ---------- Main ----------
int main() {
    std::cout << "Running " << kNumGenerators << " generators and "
              << kNumComparators << " comparators for "
              << kRunDuration.count() << " seconds...\n";

    std::vector<std::jthread> comparators;
    std::vector<std::jthread> generators;

    // Create ALL threads before joining any, so they run concurrently.
    // std::jthread passes its own stop_token as the first argument.
    for (int i = 1; i <= kNumComparators; ++i) comparators.emplace_back(comparator, i);
    for (int i = 1; i <= kNumGenerators;  ++i) generators.emplace_back(generator, i);

    std::this_thread::sleep_for(kRunDuration);

    // Two-phase shutdown so no generated number is left unprocessed.
    // Phase 1: stop the generators and wait until they have finished.
    std::osyncstream(std::cout) << "Stopping generators...\n";
    for (auto& g : generators) g.request_stop();
    for (auto& g : generators) g.join();

    // Phase 2: stop the comparators. They drain any numbers still in the queue,
    // then exit. request_stop() also wakes comparators sleeping in cv.wait().
    for (auto& c : comparators) c.request_stop();
    for (auto& c : comparators) c.join();

    // If synchronization is correct, these two numbers are always equal.
    std::cout << "Total generated: " << total_generated << '\n'
              << "Total processed: " << total_processed << '\n';
    return 0;
}
