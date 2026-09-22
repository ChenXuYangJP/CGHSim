#include "cgh/wire.hpp"
#include "solver/CudaPointFocus.hpp"
#include "reconstruction/CudaReconstruction.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
using Clock = std::chrono::steady_clock;
namespace wire = cgh::wire;
static_assert(std::atomic<bool>::is_always_lock_free, "Signal handler requires a lock-free stop flag");
std::atomic<bool> stop_requested{false};
void Stop(int) { stop_requested.store(true, std::memory_order_relaxed); }

struct Socket {
    int fd = -1;
    explicit Socket(int value) : fd(value) {}
    ~Socket() { if (fd >= 0) ::close(fd); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
};
struct Settings {
    int port = 7000, delay_ms = 0, io_timeout_ms = 30000, max_clients = 8;
    bool dummy = false;
};

bool Wait(int fd, short events, Clock::time_point deadline, const std::atomic<bool>* cancelled) {
    while (!stop_requested.load(std::memory_order_relaxed) && !wire::IsCancelled(cancelled) && Clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        pollfd p{fd, events, 0};
        const int result = ::poll(&p, 1, static_cast<int>(std::min<std::int64_t>(100, std::max<std::int64_t>(1, remaining))));
        if (result > 0) return (p.revents & events) != 0;
        if (result < 0 && errno != EINTR) return false;
    }
    return false;
}

bool Transfer(int fd, std::uint8_t* data, std::size_t size, bool writing, Clock::time_point deadline, const std::atomic<bool>* cancelled = nullptr) {
    std::size_t offset = 0;
    while (offset < size) {
        if (wire::IsCancelled(cancelled)) return false;
        if (!Wait(fd, writing ? POLLOUT : POLLIN, deadline, cancelled)) return false;
        const std::size_t chunk = std::min<std::size_t>(size - offset, 64 * 1024);
        const auto count = writing ? ::send(fd, data + offset, chunk, MSG_NOSIGNAL) : ::recv(fd, data + offset, chunk, 0);
        if (count > 0) offset += static_cast<std::size_t>(count);
        else if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) return false;
    }
    return true;
}

bool SendFrame(int fd, wire::Type type, std::uint64_t id, std::vector<std::uint8_t>& payload, Clock::time_point deadline, const std::atomic<bool>* cancelled = nullptr) {
    std::vector<std::uint8_t> header;
    std::string error;
    wire::Header h; h.type = type; h.request_id = id; h.payload_size = payload.size();
    return wire::EncodeHeader(h, header, error) && Transfer(fd, header.data(), header.size(), true, deadline, cancelled) &&
        Transfer(fd, payload.data(), payload.size(), true, deadline, cancelled);
}
void SendError(int fd, std::uint64_t id, const std::string& message, const Settings& settings) {
    if (!id) return;
    std::vector<std::uint8_t> payload;
    std::string error;
    if (wire::EncodeError(message, payload, error))
        SendFrame(fd, wire::Type::Error, id, payload, Clock::now() + std::chrono::milliseconds(settings.io_timeout_ms));
}

// During work, any inbound frame must be Cancel for this job. Incomplete Cancel
// bytes never delay cancellation; a disconnected peer is authoritative.
class Cancellation {
public:
    Cancellation(int fd, std::uint64_t id) : fd_(fd), id_(id) {}
    bool Requested() {
        if (stop_requested.load(std::memory_order_relaxed)) return true;
        for (;;) {
            const auto count = ::recv(fd_, bytes_ + received_, wire::kHeaderSize - received_, MSG_DONTWAIT);
            if (count == 0) return true;
            if (count < 0) return errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR;
            received_ += static_cast<std::size_t>(count);
            if (received_ == wire::kHeaderSize) {
                wire::Header h;
                std::string error;
                if (!wire::DecodeHeader(bytes_, sizeof(bytes_), h, error) || h.type != wire::Type::Cancel || h.request_id != id_)
                    std::cerr << "Rejected unexpected frame during job " << id_ << '\n';
                return true;
            }
        }
    }
private:
    int fd_;
    std::uint64_t id_;
    std::uint8_t bytes_[wire::kHeaderSize]{};
    std::size_t received_ = 0;
};

class CancellationMonitor {
public:
    std::atomic<bool> requested{false};
    CancellationMonitor(int fd, std::uint64_t id) : thread_([this, fd, id] {
        Cancellation cancellation(fd, id);
        while (running_.load(std::memory_order_relaxed)) {
            if (cancellation.Requested()) { requested.store(true, std::memory_order_relaxed); return; }
            pollfd p{fd, POLLIN, 0};
            ::poll(&p, 1, 10);
        }
    }) {}
    ~CancellationMonitor() { running_.store(false, std::memory_order_relaxed); thread_.join(); }
private:
    std::atomic<bool> running_{true};
    std::thread thread_;
};

void HandleClient(int fd, Settings settings) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(settings.io_timeout_ms);
    std::uint8_t header_bytes[wire::kHeaderSize];
    if (!Transfer(fd, header_bytes, sizeof(header_bytes), false, deadline)) return;
    wire::Header header;
    std::string error;
    if (!wire::DecodeHeader(header_bytes, sizeof(header_bytes), header, error)) {
        SendError(fd, header.request_id, error, settings); return;
    }
    if (header.type != wire::Type::Request && header.type != wire::Type::ReconstructionRequest &&
        header.type != wire::Type::CameraReconstructionRequest) {
        SendError(fd, header.request_id, "First frame must be a solver, observer or camera reconstruction request", settings); return;
    }
    // Receive incrementally; a header alone cannot force a 256 MiB allocation.
    std::vector<std::uint8_t> payload;
    while (payload.size() < header.payload_size) {
        std::uint8_t chunk[64 * 1024];
        const auto size = static_cast<std::size_t>(std::min<std::uint64_t>(sizeof(chunk), header.payload_size - payload.size()));
        if (!Transfer(fd, chunk, size, false, deadline)) return;
        payload.insert(payload.end(), chunk, chunk + size);
    }
    CancellationMonitor monitor(fd, header.request_id);
    const auto cancelled = [&monitor] { return monitor.requested.load(std::memory_order_relaxed); };
    const bool reconstructing = header.type == wire::Type::ReconstructionRequest;
    const bool camera_reconstructing = header.type == wire::Type::CameraReconstructionRequest;
    wire::Request request;
    wire::ReconstructionRequest reconstruction_request;
    wire::CameraReconstructionRequest camera_request;
    const bool decoded = camera_reconstructing
        ? wire::DecodeCameraReconstructionRequest(payload.data(), payload.size(), camera_request, error, &monitor.requested)
        : reconstructing
        ? wire::DecodeReconstructionRequest(payload.data(), payload.size(), reconstruction_request, error, &monitor.requested)
        : wire::DecodeRequest(payload.data(), payload.size(), request, error, &monitor.requested);
    if (!decoded) {
        if (cancelled()) return;
        SendError(fd, header.request_id, error, settings); return;
    }
    std::vector<std::uint8_t>().swap(payload);
    if ((reconstructing || camera_reconstructing) && settings.dummy) {
        if (!cancelled()) SendError(fd, header.request_id, "Optical reconstruction requires --solver cuda; the dummy fixture does not reconstruct fields", settings);
        return;
    }
    const auto delay_end = Clock::now() + std::chrono::milliseconds(settings.delay_ms);
    while (Clock::now() < delay_end) {
        if (cancelled()) return;
        pollfd p{fd, POLLIN, 0};
        ::poll(&p, 1, 10);
    }
    if (cancelled()) return;
    bool encoded = false;
    if (camera_reconstructing) {
        wire::CameraReconstructionResult result;
        if (cgh::reconstruction::CudaReconstruction::Solve(camera_request, result, error, monitor.requested) && !cancelled())
            encoded = wire::EncodeCameraReconstructionResult(result, payload, error, &monitor.requested);
    } else if (reconstructing) {
        wire::ReconstructionResult result;
        if (cgh::reconstruction::CudaReconstruction::Solve(reconstruction_request, result, error, monitor.requested) && !cancelled())
            encoded = wire::EncodeReconstructionResult(result, payload, error, &monitor.requested);
    } else {
        wire::Result result;
        const bool solved = settings.dummy
            ? cgh::solver::DummyPointFocus::Solve(request, result, error, monitor.requested)
            : cgh::solver::CudaPointFocus::Solve(request, result, error, monitor.requested);
        if (solved && !cancelled()) encoded = wire::EncodeResult(result, payload, error, &monitor.requested);
    }
    if (cancelled()) return;
    if (!encoded) {
        SendError(fd, header.request_id, error, settings);
        return;
    }
    SendFrame(fd, camera_reconstructing ? wire::Type::CameraReconstructionResult :
        reconstructing ? wire::Type::ReconstructionResult : wire::Type::Result, header.request_id, payload,
        Clock::now() + std::chrono::milliseconds(settings.io_timeout_ms), &monitor.requested);
}

bool ParseNumber(const char* text, int& value) {
    char* end = nullptr;
    errno = 0;
    const long parsed = std::strtol(text, &end, 10);
    if (errno || !end || *end || end == text || parsed < 0 || parsed > 2147483647) return false;
    value = static_cast<int>(parsed); return true;
}
}

int main(int argc, char** argv) {
    Settings settings;
    for (int i = 1; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--help") {
            std::cout << "cgh_v100_server [--port 7000] [--solver cuda|dummy] [--delay-ms 0] [--io-timeout-ms 30000] [--max-clients 8]\n"
                         "CGHV 1.4 CUDA PointFocus/PointFocusInverseR solvers and observer-plane/thin-lens camera reconstruction; --solver dummy enables only the solver transport fixture.\n";
            return 0;
        }
        if (option == "--solver") {
            if (i + 1 >= argc) { std::cerr << "Missing solver name\n"; return 2; }
            const std::string solver = argv[++i];
            if (solver != "cuda" && solver != "dummy") { std::cerr << "Solver must be cuda or dummy\n"; return 2; }
            settings.dummy = solver == "dummy";
            continue;
        }
        int value;
        if (i + 1 >= argc || !ParseNumber(argv[++i], value)) { std::cerr << "Invalid option value\n"; return 2; }
        if (option == "--port" && value <= 65535) settings.port = value;
        else if (option == "--delay-ms" && value <= 300000) settings.delay_ms = value;
        else if (option == "--io-timeout-ms" && value > 0 && value <= 300000) settings.io_timeout_ms = value;
        else if (option == "--max-clients" && value > 0 && value <= 128) settings.max_clients = value;
        else { std::cerr << "Unknown or invalid option: " << option << '\n'; return 2; }
    }
    std::signal(SIGINT, Stop); std::signal(SIGTERM, Stop); std::signal(SIGPIPE, SIG_IGN);
    Socket listener(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (listener.fd < 0) { std::perror("socket"); return 1; }
    int reuse = 1; ::setsockopt(listener.fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_ANY); address.sin_port = htons(static_cast<std::uint16_t>(settings.port));
    if (::bind(listener.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || ::listen(listener.fd, settings.max_clients)) { std::perror("bind/listen"); return 1; }
    socklen_t address_size = sizeof(address);
    if (::getsockname(listener.fd, reinterpret_cast<sockaddr*>(&address), &address_size)) { std::perror("getsockname"); return 1; }
    std::cout << "LISTENING " << ntohs(address.sin_port) << std::endl;
    struct Client { std::thread thread; std::shared_ptr<std::atomic<bool>> done; };
    std::vector<Client> clients;
    const auto reap_finished_clients = [&clients] {
        for (auto it = clients.begin(); it != clients.end();) {
            if (it->done->load(std::memory_order_acquire)) { it->thread.join(); it = clients.erase(it); } else ++it;
        }
    };
    while (!stop_requested.load(std::memory_order_relaxed)) {
        reap_finished_clients();
        pollfd p{listener.fd, POLLIN, 0};
        const auto ready = ::poll(&p, 1, 100);
        if (ready < 0 && errno != EINTR) { std::perror("poll"); break; }
        if (ready <= 0 || !(p.revents & POLLIN)) continue;
        const int fd = ::accept4(listener.fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) continue;
        // A job may finish while poll/accept waits. Reap again before admission
        // so a closed previous connection does not consume the next job's slot.
        reap_finished_clients();
        if (clients.size() >= static_cast<std::size_t>(settings.max_clients)) { ::close(fd); continue; }
        auto done = std::make_shared<std::atomic<bool>>(false);
        clients.push_back(Client{std::thread([fd, settings, done] {
            Socket socket(fd);
            try { HandleClient(fd, settings); }
            catch (const std::exception& error) { std::cerr << "Client failed: " << error.what() << '\n'; }
            // Publish completion before closing the socket. A client that sees
            // EOF can then reconnect without racing the active-client count.
            done->store(true, std::memory_order_release);
        }), done});
    }
    stop_requested.store(true, std::memory_order_relaxed);
    for (auto& client : clients) client.thread.join();
    return 0;
}
