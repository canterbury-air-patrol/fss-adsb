#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <iostream>
#include <csignal>
#include <cstdlib>
#include <memory>
#include <optional>

#include <string>
#include <string_view>
#include <unistd.h>

#include "fss.hpp"
#include <fss-log.hpp>

#include "adsb_record.hpp"
#include "aircraft_registry.hpp"
#include "args.hpp"
#include "dump1090.hpp"
#include "fss-reporter.hpp"
#include "report_queue.hpp"
#include "reporting_worker.hpp"
#include "shutdown_signal.hpp"

/* Log component/category for this module. */
constexpr const char *log_component = "adsb";

std::shared_ptr<fss_reporter_client> fss;

/* At most one queued position per distinct ICAO address (see
 * report_queue.hpp); generous versus any realistic simultaneous
 * distinct-aircraft count for one receiver, while still bounding worst-case
 * memory deterministically. handle_adsb_data() is a plain callback (see
 * notify_dump1090_adsb_data_cb), so this has to be reachable as a global,
 * same as g_aircraft_registry and fss below. Constructed in main() (like
 * fss), not here: report_queue's constructor is not noexcept, and a global
 * with static storage duration whose constructor might throw can abort
 * before main() even starts, with no way to catch it. A default-constructed
 * unique_ptr never throws. */
constexpr size_t report_queue_capacity = 4096;
std::unique_ptr<report_queue> g_report_queue;

aircraft_registry g_aircraft_registry;

void handle_adsb_data(const ADSBData &adsb)
{
    FSS_LOG_DEBUG(log_component, "ADSB Data for " << std::uppercase << std::hex << adsb.getICAOAddress());
    auto report = g_aircraft_registry.fold(adsb);
    if (report)
    {
        FSS_LOG_DEBUG(log_component, "Queueing report for " << std::uppercase << std::hex << report->icao_address
                                                            << " (" << report->callsign << ")");
        /* The message carries the timestamp of the recv() that produced it.
         * Using it for the report (rather than stamping "now" here) keeps
         * the report honest when the reporting worker's queue backs up: a
         * position that queued for minutes must not be reported as seen 0
         * seconds ago, now. */
        g_report_queue->push(report->icao_address, pending_report{*report, adsb.getLastSeen()});
    }
}

void evict_stale_aircraft()
{
    constexpr uint64_t stale_window_ms = UINT64_C(10) * 60 * 1000;
    uint64_t now = flight_safety_system::fss_current_timestamp();
    for (uint32_t address : g_aircraft_registry.evict_stale(now, stale_window_ms))
    {
        FSS_LOG_INFO(log_component, "Evicting stale aircraft " << std::uppercase << std::hex << address);
    }
}

constexpr const char *usage_args =
    " dump1090-host dump1090-port fss-host fss-port ca.public.key private.key public.key";

auto main(int argc, char *argv[]) -> int
{
    if (argc == 2)
    {
        std::string_view arg{argv[1]};
        if (arg == "--help" || arg == "-h")
        {
            std::cout << "Usage: " << argv[0] << usage_args << "\n";
            return EXIT_SUCCESS;
        }
        if (arg == "--version")
        {
            std::cout << "fss-adsb " << PACKAGE_VERSION << "\n";
            return EXIT_SUCCESS;
        }
    }

    constexpr int required_args = 8;
    if (argc != required_args)
    {
        std::cerr << "Usage: " << argv[0] << usage_args << "\n";
        return EXIT_FAILURE;
    }

    auto dump1090_port = args::parse_port(argv[2]);
    if (!dump1090_port)
    {
        std::cerr << "Invalid dump1090 port '" << argv[2] << "' (must be 1-65535)\n";
        return EXIT_FAILURE;
    }
    auto fss_port = args::parse_port(argv[4]);
    if (!fss_port)
    {
        std::cerr << "Invalid FSS port '" << argv[4] << "' (must be 1-65535)\n";
        return EXIT_FAILURE;
    }

    /* Watch out for sigint and systemd's default stop signal */
    signal(SIGINT, sigIntHandler);
    signal(SIGTERM, sigIntHandler);
    /* Ignore SIGPIPE */
    signal(SIGPIPE, SIG_IGN);

    /* Connect to FSS Server */
    fss = std::make_shared<fss_reporter_client>(argv[3], *fss_port, argv[5], argv[6], argv[7]);
    g_report_queue = std::make_unique<report_queue>(report_queue_capacity);

    /* The reporting worker must be constructed before dumper below (and so,
     * torn down after it -- see dumper.disconnect()/worker.stop() at the end
     * of this function): dumper's destructor joins the receive thread first,
     * guaranteeing no more g_report_queue->push() calls, before worker's
     * destructor shuts the queue down and joins the reporting thread, so
     * every report that is ever queued gets a chance to be sent (or at least
     * attempted) before the worker stops. */
    reporting_worker worker(*fss, *g_report_queue);

    /* Connect to Dump1090. The callback goes in via the constructor so it is
     * in place before the receive thread can deliver the first message. */
    dump1090 dumper(argv[1], *dump1090_port, handle_adsb_data);

    constexpr int evict_interval_secs = 60;
    int seconds_elapsed = 0;
    while (running)
    {
        sleep(1);
        dumper.reconnect();
        fss->attemptReconnect();
        if (++seconds_elapsed >= evict_interval_secs)
        {
            seconds_elapsed = 0;
            evict_stale_aircraft();
        }
    }

    /* Explicit, in this order: dumper first (stop ingestion, no more
     * pushes), then worker (drain whatever is left, then stop). Both
     * destructors repeat this idempotently as a backstop on any other return
     * path (there is none today, but a future early return must not silently
     * skip this ordering). */
    dumper.disconnect();
    worker.stop();

    return 0;
}
