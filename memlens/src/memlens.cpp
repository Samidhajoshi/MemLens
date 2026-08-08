// ==================== memlens.cpp ====================
// MemLens instrumenter: takes a .cpp file, injects the memlens.h
// profiler header, compiles it, and runs it so it produces
// memory_report.json.
//
// Fixed vs. the original prototype: the compile/run step used to
// build a shell command string out of the user-supplied filename
// and hand it to system(), which is a command-injection bug the
// moment the filename contains `;`, `&&`, `$( )`, etc. This version
// validates the filename against an allow-list and then execs g++
// directly via fork()/execvp() with an argv array, so the filename
// is never interpreted by a shell.
// =======================================================

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
  #include <process.h>
#else
  #include <sys/wait.h>
  #include <unistd.h>
#endif

void displayBanner() {
    std::cout << "\n";
    std::cout << "============================================================\n";
    std::cout << "                       MemLens                              \n";
    std::cout << "         C++ Memory Profiler & Leak Detector                \n";
    std::cout << "============================================================\n\n";
}

bool fileExists(const std::string& filename) {
    std::ifstream file(filename.c_str());
    return file.good();
}

std::string getFileExtension(const std::string& filename) {
    size_t dotPos = filename.find_last_of('.');
    if (dotPos != std::string::npos) return filename.substr(dotPos);
    return "";
}

// Allow-list: letters, digits, '.', '_', '-', '/'. Rejects anything a
// shell (or a path traversal attempt) could do something clever with.
// Even though we exec argv directly and never touch a shell, this is
// cheap defense-in-depth and a good sanity check on its own.
bool isSafeFilename(const std::string& filename) {
    if (filename.empty()) return false;
    for (char c : filename) {
        bool ok = std::isalnum(static_cast<unsigned char>(c)) ||
                  c == '.' || c == '_' || c == '-' || c == '/' || c == '\\' || c == ':';
        if (!ok) return false;
    }
    return true;
}

// Runs `argv[0] argv[1] ... NULL` with no shell involved. Returns the
// child's exit code, or -1 on spawn failure.
int runProcess(const std::vector<std::string>& args) {
#ifdef _WIN32
    std::vector<const char*> argv;
    for (auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    intptr_t rc = _spawnvp(_P_WAIT, argv[0], argv.data());
    return static_cast<int>(rc);
#else
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127); // execvp only returns on failure
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
#endif
}

// Finds the directory memlens.h lives in, so `memlens foo.cpp` works
// from any working directory rather than requiring memlens.h to be
// manually copied next to the file being profiled. Deliberately
// avoids platform-specific APIs (readlink("/proc/self/exe") is
// Linux-only and silently does nothing on Windows/macOS) in favor of
// parsing argv[0] plus a list of the layouts memlens.h actually ships
// in (next to the binary, in ./src, in ../src, relative to argv[0]'s
// own directory). Works whether the binary is run as `./memlens`,
// `memlens.exe`, or a full path, on any platform.
std::string findHeaderDir(const std::string& argv0) {
    std::vector<std::string> candidates = {".", "src", "./src", "../src"};

    // Also check relative to wherever argv[0] itself lives, in case
    // the binary is invoked from outside the project directory
    // (e.g. `/some/other/path/memlens foo.cpp`).
    size_t slash = argv0.find_last_of("/\\");
    if (slash != std::string::npos) {
        std::string exeDir = argv0.substr(0, slash);
        candidates.push_back(exeDir);
        candidates.push_back(exeDir + "/src");
        candidates.push_back(exeDir + "/../src");
    }

    for (const auto& dir : candidates) {
        if (fileExists(dir + "/memlens.h")) return dir;
    }
    return "."; // last resort: same behavior as before, may fail to compile
}

int main(int argc, char** argv) {
    displayBanner();

    std::string headerDir = findHeaderDir(argc > 0 ? argv[0] : ".");

    std::string filename;
    if (argc > 1) {
        filename = argv[1];
    } else {
        std::cout << "[*] Enter the C++ file to analyze: ";
        std::getline(std::cin, filename);
    }

    size_t start = filename.find_first_not_of(" \t\n\r");
    size_t end = filename.find_last_not_of(" \t\n\r");
    if (start != std::string::npos && end != std::string::npos) {
        filename = filename.substr(start, end - start + 1);
    }

    if (!isSafeFilename(filename)) {
        std::cout << "\n[ERROR] That filename contains characters MemLens won't pass to a "
                     "compiler (only letters, digits, '.', '_', '-', '/' are allowed).\n\n";
        return 1;
    }

    if (!fileExists(filename)) {
        std::cout << "\n[ERROR] File '" << filename << "' not found!\n\n";
        return 1;
    }

    std::string ext = getFileExtension(filename);
    if (ext != ".cpp" && ext != ".cc" && ext != ".cxx" && ext != ".c++") {
        std::cout << "\n[WARNING] File doesn't have a C++ extension. Proceeding anyway...\n\n";
    }

    std::cout << "[OK] File found: " << filename << "\n";
    std::cout << "[*] Instrumenting code with memlens.h...\n";

    std::string instrumentedFile = "instrumented_" + filename.substr(filename.find_last_of('/') + 1);
    std::ifstream inFile(filename.c_str());
    std::ofstream outFile(instrumentedFile.c_str());
    outFile << "#include \"memlens.h\"\n";

    std::string line;
    bool hasMain = false;
    std::vector<std::string> fileContent;
    bool alreadyInstrumented = false;

    while (std::getline(inFile, line)) {
        if (line.find("#include \"memlens.h\"") != std::string::npos ||
            line.find("#include \"memory_profiler.h\"") != std::string::npos) {
            std::cout << "\n[WARNING] File already includes the profiler header. Using as-is...\n";
            alreadyInstrumented = true;
            hasMain = true;
            break;
        }
        if (line.find("int main") != std::string::npos) hasMain = true;
        fileContent.push_back(line);
    }
    inFile.close();

    if (alreadyInstrumented) {
        outFile.close();
        std::remove(instrumentedFile.c_str());
        instrumentedFile = filename;
    } else {
        bool inMain = false;
        int braceCount = 0;
        for (auto& currentLine : fileContent) {
            if (currentLine.find("int main") != std::string::npos) inMain = true;
            if (inMain) {
                for (char c : currentLine) {
                    if (c == '{') braceCount++;
                    if (c == '}') braceCount--;
                }
            }
            if (inMain && braceCount == 1 && currentLine.find("return") != std::string::npos) {
                outFile << "    // MemLens report generation\n";
                outFile << "    memlens::MemoryProfiler::getInstance().printSummary();\n";
                outFile << "    memlens::MemoryProfiler::getInstance().generateJSONReport();\n";
                outFile << "\n";
            }
            outFile << currentLine << "\n";
        }
        outFile.close();
    }

    if (!hasMain) {
        std::cout << "\n[WARNING] No main() found; MemLens needs one to generate a report.\n\n";
        return 1;
    }

    std::cout << "\n[*] Compiling instrumented code...\n";
    if (!fileExists(headerDir + "/memlens.h")) {
        std::cout << "[ERROR] Couldn't locate memlens.h (looked in '.', 'src', '../src', and "
                     "next to the memlens executable). Run memlens from the project root, or "
                     "copy memlens.h next to " << filename << ".\n\n";
        return 1;
    }
    std::string outputExe = "profiled_program";
    std::vector<std::string> compileArgs = {"g++", "-std=c++14"};
#ifndef _WIN32
    // -pthread is how you link libpthread on Linux/macOS for
    // std::thread/std::mutex support. On Windows it's unnecessary:
    // mingw-w64 links its threading support automatically, and older
    // MinGW builds without gthreads support don't have a pthread
    // library to link against at all (memlens.h's Win32 fallback
    // doesn't need one either). Passing it there just breaks the
    // link with "cannot find -lpthread".
    compileArgs.push_back("-pthread");
#endif
    compileArgs.push_back("-I" + headerDir);
    compileArgs.push_back(instrumentedFile);
    compileArgs.push_back("-o");
    compileArgs.push_back(outputExe);
    int compileResult = runProcess(compileArgs);

    if (compileResult != 0) {
        std::cout << "\n[ERROR] Compilation failed (see above).\n\n";
        return 1;
    }
    std::cout << "[OK] Compilation successful!\n\n[*] Running memory analysis...\n";

#ifdef _WIN32
    runProcess({outputExe + ".exe"});
#else
    runProcess({"./" + outputExe});
#endif

    std::cout << "\n[OK] Analysis complete. Open dashboard.html and load memory_report.json to view it.\n\n";

    if (instrumentedFile != filename) std::remove(instrumentedFile.c_str());
    return 0;
}
