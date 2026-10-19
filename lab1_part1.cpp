#include <iostream>
#include <thread>
#include <chrono>
#include <random>
#include <syncstream>
#include <mutex>
#include <condition_variable>

using namespace std::chrono_literals;   // lets us write 1s, 15s instead of std::chrono::seconds(1)

// ---------- Configurations ----------
constexpr auto kRunDuration = 15s;      // how long the program runs before stopping

// ---------- Shared data ----------
// Both threads read and write these, so they need protection.
std::mutex mtx;                 // only one thread can hold this at a time
std::condition_variable_any cv; // lets a thread sleep until the state changes (works with stop_token)

int shared_variable = 0;        // the number the generator puts down for the comparator
bool has_value = false;         // true = a number is waiting, false = it's been taken

void generator(std::stop_token stop){
    // each thread gets its own random engine, since engines aren't thread-safe
    std::random_device rd;
    std::mt19937 engine{rd()};
    std::uniform_int_distribution<int> dist{0, 20};

    while (!stop.stop_requested()) {
        const int value = dist(engine);   // make the number (no lock needed, it's local)

        {   // critical section: wait for our turn, then publish the number
            std::unique_lock<std::mutex> lock(mtx);

            // wait until the comparator has taken the last number
            // returns false if main asked us to stop while we were waiting
            if (!cv.wait(lock, stop, [] { return !has_value; })) return;

            shared_variable = value;
            has_value = true;           // tell the comparator there's a new number
        }   // lock released here

        cv.notify_all();                // wake the comparator so it can read the number

        // printing outside the lock so we don't hold the mutex while doing slow output
        std::osyncstream(std::cout) << "Generator generated: " << value << "\n";
        std::this_thread::sleep_for(1s); // generate once per second, as the lab asks
    }
}

void comparator(std::stop_token stop){
    while (!stop.stop_requested()) {
        int value;   // declared out here so it's still visible after the lock block
        {   // critical section: wait for a number, then take it
            std::unique_lock<std::mutex> lock(mtx);

            // sleep until has_value is true (a number is waiting)
            // returns false if we're being stopped
            if (!cv.wait(lock, stop, [] { return has_value; })) return;

            value = shared_variable;
            has_value = false;          // mark it as taken so we don't read it twice
        }   // lock released here

        cv.notify_all();                // tell the generator the slot is free again

        // build and print the result line
        std::osyncstream out(std::cout);
        out << "Comparator: Number " << value;

        if(value > 10) {
            out << " is greater than 10\n";
        } else if (value < 10) {
            out << " is less than 10\n";
        }
        else {
            out << " is equal to 10\n";
        }
    }
}

int main() {
    std::cout << "Running for " << kRunDuration.count() << " seconds...\n";

    // jthread starts running as soon as it's created
    std::jthread generator_thread(generator);
    std::jthread comparator_thread(comparator);

    std::this_thread::sleep_for(kRunDuration);  // main just waits while the threads work

    std::cout << "Stopping threads...\n";
    generator_thread.request_stop();    // sets the stop flag, wakes any sleeping thread
    comparator_thread.request_stop();
    return 0;   // jthread destructors join the threads before the program exits
}