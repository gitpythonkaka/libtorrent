/*

Copyright (c) 2015, Arvid Norberg
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the distribution.
    * Neither the name of the author nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

*/

#include <array>
#include "test.hpp"
#include "create_torrent.hpp"
#include "settings.hpp"
#include "libtorrent/session.hpp"
#include "libtorrent/deadline_timer.hpp"
#include "libtorrent/settings_pack.hpp"
#include "libtorrent/ip_filter.hpp"
#include "libtorrent/alert_types.hpp"
#include "simulator/simulator.hpp"

using namespace sim;

namespace lt = libtorrent;

struct fake_peer
{
	fake_peer(simulation& sim, char const* ip)
		: m_ios(sim, asio::ip::address::from_string(ip))
		, m_acceptor(m_ios)
		, m_socket(m_ios)
		, m_tripped(false)
	{
		boost::system::error_code ec;
		m_acceptor.open(asio::ip::tcp::v4(), ec);
		TEST_CHECK(!ec);
		m_acceptor.bind(asio::ip::tcp::endpoint(asio::ip::address_v4::any(), 6881), ec);
		TEST_CHECK(!ec);
		m_acceptor.listen(10, ec);
		TEST_CHECK(!ec);

		m_acceptor.async_accept(m_socket, [&] (boost::system::error_code const& ec)
			{ if (!ec) m_tripped = true; });
	}

	void close()
	{
		m_acceptor.close();
		m_socket.close();
	}

	bool tripped() const { return m_tripped; }

private:
	asio::io_service m_ios;
	asio::ip::tcp::acceptor m_acceptor;
	asio::ip::tcp::socket m_socket;
	bool m_tripped;
};

template <typename Setup, typename HandleAlerts, typename Test>
void run_test(Setup const& setup
	, HandleAlerts const& on_alert
	, Test const& test)
{
	// setup the simulation
	sim::default_config network_cfg;
	sim::simulation sim{network_cfg};
	sim::asio::io_service ios(sim, asio::ip::address_v4::from_string("50.0.0.1"));
	lt::session_proxy zombie;

	// setup settings pack to use for the session (customization point)
	lt::settings_pack pack = settings();
	// create session
	std::shared_ptr<lt::session> ses = std::make_shared<lt::session>(pack, ios);

	fake_peer p1(sim, "60.0.0.0");
	fake_peer p2(sim, "60.0.0.1");
	fake_peer p3(sim, "60.0.0.2");
	fake_peer p4(sim, "60.0.0.3");
	fake_peer p5(sim, "60.0.0.4");
	std::array<fake_peer*, 5> test_peers = {{ &p1, &p2, &p3, &p4, &p5 }};

	// set up test, like adding torrents (customization point)
	setup(*ses);

	// the alert notification function is called from within libtorrent's
	// context. It's not OK to talk to libtorrent in there, post it back out and
	// then ask for alerts.
	ses->set_alert_notify([&] { ios.post([&] {
		std::vector<lt::alert*> alerts;
		ses->pop_alerts(&alerts);
		// call the user handler
		if (!alerts.empty()) on_alert(*ses, alerts);
	} ); } );

	lt::deadline_timer timer(ios);
	timer.expires_from_now(lt::seconds(60));
	timer.async_wait([&](lt::error_code const& ec)
	{
		test(*ses, test_peers);

		ses->set_alert_notify([]{});
		// shut down
		zombie = ses->abort();

		for (auto* p : test_peers) p->close();

		ses.reset();
	});

	sim.run();
}

void add_fake_peers(lt::torrent_handle h)
{
	// add the fake peers
	for (int i = 0; i < 5; ++i)
	{
		char ep[30];
		snprintf(ep, sizeof(ep), "60.0.0.%d", i);
		h.connect_peer(lt::tcp::endpoint(
			lt::address_v4::from_string(ep), 6881));
	}
}

void check_tripped(std::array<fake_peer*, 5>& test_peers, std::array<bool, 5> expected)
{
	int idx = 0;
	for (auto p : test_peers)
	{
		TEST_EQUAL(p->tripped(), expected[idx]);
		++idx;
	}
}

void add_ip_filter(lt::session& ses)
{
	lt::ip_filter filter;
	// filter out 0-2 inclusive
	filter.add_rule(
		asio::ip::address_v4::from_string("60.0.0.0")
		, asio::ip::address_v4::from_string("60.0.0.2")
		, lt::ip_filter::blocked);
	ses.set_ip_filter(filter);
}

// set an IP filter, add a torrent, add peers, make sure the correct ones are
// connected to
TORRENT_TEST(apply_ip_filter)
{
	lt::time_point start_time = lt::clock_type::now();

	run_test(
		[](lt::session& ses)
		{
			add_ip_filter(ses);

			lt::add_torrent_params params = create_torrent(0, false);
			params.flags &= ~lt::add_torrent_params::flag_auto_managed;
			params.flags &= ~lt::add_torrent_params::flag_paused;
			ses.async_add_torrent(params);
		},

		[&](lt::session& ses, std::vector<lt::alert*> const& alerts)
		{
			for (lt::alert const* a : alerts)
			{
				printf("%-3d %s\n", int(lt::duration_cast<lt::seconds>(a->timestamp()
					- start_time).count()), a->message().c_str());

				if (lt::add_torrent_alert const* at = lt::alert_cast<lt::add_torrent_alert>(a))
				{
					lt::torrent_handle h = at->handle;
					add_fake_peers(h);
				}
			}
		},

		[](lt::session& ses, std::array<fake_peer*, 5>& test_peers)
		{
			check_tripped(test_peers, {{false, false, false, true, true}} );
		}
	);
}

// add a torrent, set an IP filter, add peers, make sure the correct ones are
// connected to
TORRENT_TEST(update_ip_filter)
{
	lt::time_point start_time = lt::clock_type::now();

	run_test(
		[](lt::session& ses)
		{
			lt::add_torrent_params params = create_torrent(0, false);
			params.flags &= ~lt::add_torrent_params::flag_auto_managed;
			params.flags &= ~lt::add_torrent_params::flag_paused;
			ses.async_add_torrent(params);
		},

		[&](lt::session& ses, std::vector<lt::alert*> const& alerts)
		{
			for (lt::alert const* a : alerts)
			{
				printf("%-3d %s\n", int(lt::duration_cast<lt::seconds>(a->timestamp()
					- start_time).count()), a->message().c_str());

				if (lt::add_torrent_alert const* at = lt::alert_cast<lt::add_torrent_alert>(a))
				{
					// here we add the IP filter after the torrent has already been
					// added
					add_ip_filter(ses);

					lt::torrent_handle h = at->handle;
					add_fake_peers(h);
				}
			}
		},

		[](lt::session& ses, std::array<fake_peer*, 5>& test_peers)
		{
			check_tripped(test_peers, {{false, false, false, true, true}} );
		}
	);
}

TORRENT_TEST(apply_ip_filter_to_torrent)
{
	lt::time_point start_time = lt::clock_type::now();

	run_test(
		[](lt::session& ses)
		{
			add_ip_filter(ses);

			lt::add_torrent_params params = create_torrent(0, false);
			params.flags &= ~lt::add_torrent_params::flag_auto_managed;
			params.flags &= ~lt::add_torrent_params::flag_paused;

			// disable the IP filter!
			params.flags &= ~lt::add_torrent_params::flag_apply_ip_filter;
			ses.async_add_torrent(params);
		},

		[&](lt::session& ses, std::vector<lt::alert*> const& alerts)
		{
			for (lt::alert const* a : alerts)
			{
				printf("%-3d %s\n", int(lt::duration_cast<lt::seconds>(a->timestamp()
					- start_time).count()), a->message().c_str());

				if (lt::add_torrent_alert const* at = lt::alert_cast<lt::add_torrent_alert>(a))
				{
					lt::torrent_handle h = at->handle;
					add_fake_peers(h);
				}
			}
		},

		[](lt::session& ses, std::array<fake_peer*, 5>& test_peers)
		{
			// since the IP filter didn't apply to this torrent, it should have hit
			// all peers
			check_tripped(test_peers, {{true, true, true, true, true}} );
		}
	);
}

// make sure IP filters apply to trackers
TORRENT_TEST(ip_filter_trackers)
{
	lt::time_point start_time = lt::clock_type::now();

	run_test(
		[](lt::session& ses)
		{
			add_ip_filter(ses);

			lt::add_torrent_params params = create_torrent(0, false);
			params.flags &= ~lt::add_torrent_params::flag_auto_managed;
			params.flags &= ~lt::add_torrent_params::flag_paused;
			params.trackers = {
				"http://60.0.0.0:6881/announce"
				, "http://60.0.0.1:6881/announce"
				, "http://60.0.0.2:6881/announce"
				, "http://60.0.0.3:6881/announce"
				, "http://60.0.0.4:6881/announce"
				};
			ses.async_add_torrent(params);
		},

		[&](lt::session& ses, std::vector<lt::alert*> const& alerts)
		{
			for (lt::alert const* a : alerts)
			{
				printf("%-3d %s\n", int(lt::duration_cast<lt::seconds>(a->timestamp()
					- start_time).count()), a->message().c_str());
			}
		},

		[](lt::session& ses, std::array<fake_peer*, 5>& test_peers)
		{
			check_tripped(test_peers, {{false, false, false, true, true}} );
		}
	);
}

