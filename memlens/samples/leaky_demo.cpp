#include <iostream>

class MyClass {
    int* data;
public:
    MyClass(int size) {
        data = new int[size];
    }
    // Missing destructor - LEAK!
};

int main() {
    int* leak1 = new int[100];
    char* leak2 = new char[500];
    
    MyClass* obj = new MyClass(200);
    
    int* noLeak = new int[50];
    delete[] noLeak;
    
    return 0;
}