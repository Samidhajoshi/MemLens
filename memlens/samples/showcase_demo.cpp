// Deliberately buggy program used to generate the sample
// memory_report.json shipped with the MemLens dashboard demo.
#include <iostream>

class Connection {
    char* buffer;
public:
    Connection() { buffer = new char[256]; }
    // missing destructor -> buffer leaks every time a Connection is made
};

int main() {
    // plain leaks
    int* userIds = new int[1000];
    char* nameBuffer = new char[128];

    // an object whose internals leak
    Connection* conn = new Connection();
    delete conn; // frees the Connection but not its internal buffer

    // double free
    int* counter = new int(0);
    delete counter;
    delete counter;

    // new[]/delete mismatch
    double* samples = new double[32];
    delete samples;

    // a couple of allocations that ARE cleaned up correctly
    for (int i = 0; i < 5; i++) {
        int* scratch = new int(i);
        delete scratch;
    }

    return 0;
}
