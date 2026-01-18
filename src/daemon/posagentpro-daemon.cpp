// posagentpro-daemon.cpp
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/poll.h>
#include <signal.h>
#include <errno.h>

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <map>
#include <chrono>
#include <thread>

static volatile sig_atomic_t keep_running = 1;

void sigint_handler(int) {
    keep_running = 0;
}

std::map<std::string, std::string> parse_conf(const std::string &path) {
    std::map<std::string, std::string> m;
    std::ifstream f(path);
    if (!f.is_open()) return m;
    std::string line;
    while (std::getline(f, line)) {
        // strip
        auto pos = line.find('#');
        if (pos != std::string::npos) line = line.substr(0, pos);
        size_t i = 0;
        while (i < line.size() && isspace((unsigned char)line[i])) ++i;
        if (i == line.size()) continue;
        auto eq = line.find('=', i);
        if (eq == std::string::npos) continue;
        std::string key = line.substr(i, eq - i);
        std::string val = line.substr(eq + 1);
        // trim
        auto trim = [](std::string &s){
            size_t a = 0;
            while (a < s.size() && isspace((unsigned char)s[a])) ++a;
            size_t b = s.size();
            while (b > a && isspace((unsigned char)s[b-1])) --b;
            s = s.substr(a, b-a);
        };
        trim(key); trim(val);
        if (!key.empty()) m[key] = val;
    }
    return m;
}

int set_interface_attribs(int fd, int speed, int parity, int databits, int stopbits) {
    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) {
        return -1;
    }

    cfsetospeed(&tty, speed);
    cfsetispeed(&tty, speed);

    tty.c_cflag &= ~CSIZE;
    switch (databits) {
        case 7: tty.c_cflag |= CS7; break;
        case 8: default: tty.c_cflag |= CS8; break;
    }

    if (parity == 0) {
        tty.c_cflag &= ~PARENB;
    } else if (parity == 1) {
        tty.c_cflag |= PARENB; tty.c_cflag &= ~PARODD;
    } else if (parity == 2) {
        tty.c_cflag |= PARENB; tty.c_cflag |= PARODD;
    }

    if (stopbits == 2) tty.c_cflag |= CSTOPB; else tty.c_cflag &= ~CSTOPB;

    tty.c_cflag |= CLOCAL | CREAD;
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    tty.c_iflag &= ~(IXON | IXOFF | IXANY | ICRNL);
    tty.c_oflag &= ~OPOST;

    // set timeout
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 10; // 1.0 seconds

    if (tcsetattr(fd, TCSANOW, &tty) != 0) return -1;
    return 0;
}

speed_t baud_from_int(int baud) {
    switch (baud) {
        case 50: return B50; case 75: return B75; case 110: return B110;
        case 134: return B134; case 150: return B150; case 200: return B200;
        case 300: return B300; case 600: return B600; case 1200: return B1200;
        case 1800: return B1800; case 2400: return B2400; case 4800: return B4800;
        case 9600: return B9600; case 19200: return B19200; case 38400: return B38400;
        case 57600: return B57600; case 115200: return B115200; case 230400: return B230400;
        default: return (speed_t) -1;
    }
}

int main(int argc, char** argv) {
    std::string config_path = "/etc/posagentpro/posagentpro.conf";
    for (int i=1;i<argc;i++) {
        std::string a = argv[i];
        if (a == "--config" && i+1 < argc) { config_path = argv[++i]; }
        else if (a == "-c" && i+1 < argc) { config_path = argv[++i]; }
        else if (a == "--help" || a=="-h") {
            std::cout << "Usage: posagentpro-daemon [--config /path/to/conf]" << std::endl;
            return 0;
        }
    }

    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    auto conf = parse_conf(config_path);
    std::string device = "/dev/ttyUSB0";
    int baud = 115200;
    int databits = 8;
    int parity = 0; // 0 none,1 even,2 odd
    int stopbits = 1;
    std::string logfile;
    if (conf.count("device")) device = conf["device"];
    if (conf.count("baud")) baud = std::stoi(conf["baud"]);
    if (conf.count("databits")) databits = std::stoi(conf["databits"]);
    if (conf.count("parity")) {
        std::string p = conf["parity"];
        if (p == "none" ) parity = 0;
        else if (p == "even") parity = 1;
        else if (p == "odd") parity = 2;
    }
    if (conf.count("stopbits")) stopbits = std::stoi(conf["stopbits"]);
    if (conf.count("logfile")) logfile = conf["logfile"];

    std::ofstream logstream;
    if (!logfile.empty()) {
        logstream.open(logfile, std::ios::app);
        if (!logstream.is_open()) {
            std::cerr << "Failed to open logfile: " << logfile << std::endl;
            // continue without logfile
        }
    }

    std::cout << "posagentpro-daemon starting. device=" << device << " baud=" << baud << std::endl;
    if (logstream.is_open()) logstream << "posagentpro-daemon starting\n";

    int fd = open(device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        std::cerr << "Failed to open device " << device << ": " << strerror(errno) << std::endl;
        return 2;
    }

    speed_t sp = baud_from_int(baud);
    if (sp == (speed_t)-1) sp = B115200;
    if (set_interface_attribs(fd, sp, parity, databits, stopbits) != 0) {
        std::cerr << "Failed to set serial attributes: " << strerror(errno) << std::endl;
        close(fd);
        return 3;
    }

    // Use poll to wait for data
    struct pollfd pfd;
    pfd.fd = fd; pfd.events = POLLIN;

    const size_t BUF_SZ = 4096;
    std::string buffer;
    buffer.reserve(BUF_SZ);
    char tmp[BUF_SZ];

    while (keep_running) {
        int rv = poll(&pfd, 1, 500); // 500ms
        if (rv < 0) {
            if (errno == EINTR) continue;
            std::cerr << "poll error: " << strerror(errno) << std::endl;
            break;
        } else if (rv == 0) {
            // timeout
            continue;
        }
        if (pfd.revents & POLLIN) {
            ssize_t n = read(fd, tmp, BUF_SZ);
            if (n > 0) {
                // write to stdout and logfile if set
                ssize_t w = write(STDOUT_FILENO, tmp, n);
                (void)w;
                if (logstream.is_open()) {
                    logstream.write(tmp, n);
                    logstream.flush();
                }
            } else if (n == 0) {
                // EOF
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                std::cerr << "read error: " << strerror(errno) << std::endl;
                break;
            }
        }
    }

    std::cout << "posagentpro-daemon stopping" << std::endl;
    if (logstream.is_open()) logstream << "posagentpro-daemon stopping\n";
    close(fd);
    if (logstream.is_open()) logstream.close();
    return 0;
}