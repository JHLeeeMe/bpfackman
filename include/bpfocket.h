#ifndef BPFOCKET_H
#define BPFOCKET_H


#include <sys/ioctl.h>     // ioctl()
#include <sys/socket.h>    // socket()
#include <net/if.h>        // struct ifconf, struct ifreq 
#include <net/if_arp.h>    // ARPHDR_ETHER
#include <net/ethernet.h>  // ETH_P_ALL
#include <netinet/in.h>    // htons()
#include <unistd.h>        // close()

#include <stdexcept>  // runtime_error()
#include <utility>    // std::pair<>

#define __BPFOCKET_BEGIN namespace bpfocket {
#define __BPFOCKET_END   }

__BPFOCKET_BEGIN

/// ============================================================================
/// Declarations
/// ============================================================================

namespace utils
{
    enum class eResultCode;

    [[noreturn]]
    void throwRuntimeError(eResultCode code,
                           const ssize_t err_no,
                           const std::string& caller_info,
                           const std::string& msg = "");
}  // ::bpfocket::utils

namespace filter
{

}  // ::bpfocket::filter

namespace core
{
    class RawSocket
    {
    public:  // rule of 5
        RawSocket();
        ~RawSocket();

        RawSocket(const RawSocket&) = delete;
        RawSocket& operator=(const RawSocket&) = delete;

        RawSocket(RawSocket&&) = delete;
        RawSocket& operator=(RawSocket&&) = delete;
    public:
        auto fd()     const -> const int;
        auto ifname() const -> std::string;
        auto err()    const -> const ssize_t;

    private:
        auto create_fd()  -> utils::eResultCode;
        auto set_ifname() -> utils::eResultCode;
        auto find_eth_ifr(const struct ifconf& ifc)
                -> std::pair<utils::eResultCode, struct ifreq>;
    private:
        int fd_;
        std::string ifname_;

        ssize_t err_;
    };
}  // ::bpfocket::core


/// ============================================================================
/// Definitions
/// ============================================================================

namespace utils
{
    enum class eResultCode
    {
        Success = 0,

        Failure           = 100,
        InterfaceNotFound = Failure + 1,  // 101

        IoctlFailureBase     = 200,
        IoctlGetConfigFailed = IoctlFailureBase + 1,  // 201
        IoctlGetFlagsFailed  = IoctlFailureBase + 2,  // 202
        IoctlSetFlagsFailed  = IoctlFailureBase + 3,  // 203
        IoctlGetHwAddrFailed = IoctlFailureBase + 4,  // 204

        SocketFailureBase     = 300,
        SocketCreationFailed  = SocketFailureBase + 1,  // 301
        SocketSetOptFailed    = SocketFailureBase + 2,  // 302
    };

    [[noreturn]]
    void throwRuntimeError(eResultCode code,
                           const ssize_t err_no,
                           const std::string& caller_info,
                           const std::string& msg)
    {
        std::ostringstream oss{};
        oss << "Error occurred in " << caller_info << ":\n\t";

        if (!msg.empty())
        {
            oss << msg;
        }

        oss << " [code: " << static_cast<uint32_t>(code) << "]";

        if (err_no != 0)
        {
            oss << "[errno: " << err_no << "]";
        }

        throw std::runtime_error(oss.str());
    }
}  // ::bpfocket::utils

namespace filter
{

}  // ::bpfocket::filter

namespace core
{
    RawSocket::RawSocket()
        : fd_{}
        , ifname_{}
        , err_{}
    {
        utils::eResultCode code{};

        if ((code = create_fd()) != utils::eResultCode::Success)
        {
            utils::throwRuntimeError(code, err_, __FUNCTION__, "create_fd()");
        }

        if ((code = set_ifname()) != utils::eResultCode::Success)
        {
            utils::throwRuntimeError(code, err_, __FUNCTION__, "set_ifname()");
        }
    }

    RawSocket::~RawSocket()
    {
        close(fd_);
    }

    auto RawSocket::fd() const -> const int
    {
        return fd_;
    }

    auto RawSocket::ifname() const -> std::string
    {
        return ifname_;
    }

    auto RawSocket::err() const -> const ssize_t
    {
        return err_;
    }

    auto RawSocket::create_fd() -> utils::eResultCode
    {
        fd_ = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
        if (fd_ < 0)
        {
            err_ = errno;
            return utils::eResultCode::SocketCreationFailed;
        }

        return utils::eResultCode::Success;
    }

    auto RawSocket::set_ifname() -> utils::eResultCode
    {
        struct ifconf ifc{};

        if (::ioctl(fd_, SIOCGIFCONF, &ifc) < 0)
        {
            err_ = errno;
            return utils::eResultCode::IoctlGetConfigFailed;
        }

        std::vector<char> buf(ifc.ifc_len);
        ifc.ifc_buf = buf.data();

        if (::ioctl(fd_, SIOCGIFCONF, &ifc) < 0)
        {
            err_ = errno;
            return utils::eResultCode::IoctlGetConfigFailed;
        }

        std::pair<utils::eResultCode, struct ifreq> result{ find_eth_ifr(ifc) };
        utils::eResultCode code{ result.first };
        if (code != utils::eResultCode::Success)
        {
            if (code == utils::eResultCode::InterfaceNotFound)
            {
                err_ = 0;
            }

            return code;
        }

        struct ifreq eth_ifr{ result.second };
        ifname_ = eth_ifr.ifr_name;

        return utils::eResultCode::Success;
    }

    auto RawSocket::find_eth_ifr(const struct ifconf& ifc)
            -> std::pair<utils::eResultCode, struct ifreq>
    {
        struct ifreq* ifr{ ifc.ifc_req };

        for (size_t i = 0; i < ifc.ifc_len / sizeof(struct ifreq); i++)
        {
            if (::ioctl(fd_, SIOCGIFFLAGS, &ifr[i]) < 0)
            {
                err_ = errno;
                return { utils::eResultCode::IoctlGetFlagsFailed, {} };
            }

            if ((ifr[i].ifr_flags & IFF_LOOPBACK) ||
                !(ifr[i].ifr_flags & IFF_UP) ||
                !(ifr[i].ifr_flags & IFF_RUNNING))
            {
                continue;
            }

            if (::ioctl(fd_, SIOCGIFHWADDR, &ifr[i]) < 0)
            {
                err_ = errno;
                return { utils::eResultCode::IoctlGetHwAddrFailed, {} };
            }

            if (ifr[i].ifr_hwaddr.sa_family != ARPHRD_ETHER)
            {
                continue;
            }

            const std::string eth_ifr_name{ ifr[i].ifr_name };
            if (eth_ifr_name.find("eth") != std::string::npos ||
                eth_ifr_name.find("en") != std::string::npos)
            {
                return { utils::eResultCode::Success, ifr[i] };
            }
        }

        return { utils::eResultCode::InterfaceNotFound, {} };
    }
}  // ::bpfocket::core
__BPFOCKET_END


#endif  // BPFOCKET_H
