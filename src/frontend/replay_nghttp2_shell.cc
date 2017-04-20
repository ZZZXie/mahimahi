/* -*-mode:c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include <net/route.h>
#include <fcntl.h>
#include <fstream>

#include <vector>
#include <set>
#include <map>
#include <sstream>
#include <chrono>
#include <thread>

#include "util.hh"
#include "netdevice.hh"
#include "web_server.hh"
#include "system_runner.hh"
#include "socket.hh"
#include "event_loop.hh"
#include "temp_file.hh"
#include "http_response.hh"
#include "dns_server.hh"
#include "exception.hh"
#include "interfaces.hh"
#include "nat.hh"
#include "socketpair.hh"
#include "squid_proxy.hh"
#include "reverse_proxy.hh"
#include "pac_file.hh"
#include "vpn.hh"

#include "http_record.pb.h"

#include "config.h"

using namespace std;

void add_dummy_interface( const string & name, const Address & addr )
{
    run( { IP, "link", "add", name, "type", "dummy" } );

    interface_ioctl( SIOCSIFFLAGS, name,
                     [] ( ifreq &ifr ) { ifr.ifr_flags = IFF_UP; } );
    interface_ioctl( SIOCSIFADDR, name,
                     [&] ( ifreq &ifr ) { ifr.ifr_addr = addr.to_sockaddr(); } );
}

void get_domains( const string & filename, set<string> & domains ) {
  cout << "Domain filename: " << filename << endl;
  ifstream infile;
  infile.open(filename);
  string line;
  while (getline(infile, line))
  {
    cout << "Domain: " << line << endl;
    domains.insert(line);
  }
}

int main( int argc, char *argv[] )
{
    try {
        /* clear environment */
        char **user_environment = environ;
        environ = nullptr;

        check_requirements( argc, argv );

        if ( argc < 8 ) {
            throw runtime_error( "Usage: " + string( argv[ 0 ] ) + " directory nghttpx_path nghttpx_port nghttpx_key nghttpx_cert vpn_port mode path_to_dependency_file" );
        }

        /* clean directory name */
        string directory = argv[ 1 ];

        if ( directory.empty() ) {
            throw runtime_error( string( argv[ 0 ] ) + ": directory name must be non-empty" );
        }

        /* Get the application variables. */
        string nghttpx_path = string(argv[ 2 ]);
        string nghttpx_key_path = string(argv[ 4 ]);
        string nghttpx_cert_path = string(argv[ 5 ]);

        /* make sure directory ends with '/' so we can prepend directory to file name for storage */
        if ( directory.back() != '/' ) {
            directory.append( "/" );
        }

        /* get working directory */
        const string working_directory { get_working_directory() };

        /* chdir to result of getcwd just in case */
        SystemCall( "chdir", chdir( working_directory.c_str() ) );

        /* set egress and ingress ip addresses */
        Address egress_addr, ingress_addr;
        tie( egress_addr, ingress_addr ) = two_unassigned_addresses();

        /* make pair of devices */
        string egress_name = "veth-" + to_string( getpid() ), ingress_name = "veth-i" + to_string( getpid() );
        VirtualEthernetPair veth_devices( egress_name, ingress_name );
        cout << "egress name: " << egress_name << " ingress name: " << ingress_name << endl;

        /* bring up egress */
        assign_address( egress_name, egress_addr, ingress_addr );

        /* set up DNAT between eth0 to ingress address. */
        int vpn_port = atoi(argv[6]);
        DNATWithPostrouting dnat( Address(ingress_addr.ip(), vpn_port), "udp", vpn_port );

        /* set up NAT between egress and eth0 */
        NAT nat_rule( ingress_addr );

        // int nghttpx_port = atoi(argv[3]);
        EventLoop outer_event_loop;

        string escaped_page = argv[9];
        
        /* Fork */
        {
          /* Make pipe for start signal */
          auto pipe = UnixDomainSocket::make_pair();

          ChildProcess container_process( "proxy-replayshell", [&]() {
              pipe.second.read();

              /* bring up localhost */
              interface_ioctl( SIOCSIFFLAGS, "lo",
                               [] ( ifreq &ifr ) { ifr.ifr_flags = IFF_UP; } );

              /* bring up veth device */
              assign_address( ingress_name, ingress_addr, egress_addr );

              /* create default route */
              rtentry route;
              zero( route );

              route.rt_gateway = egress_addr.to_sockaddr();
              route.rt_dst = route.rt_genmask = Address().to_sockaddr();
              route.rt_flags = RTF_UP | RTF_GATEWAY;

              SystemCall( "ioctl SIOCADDRT", ioctl( UDPSocket().fd_num(), SIOCADDRT, &route ) );

              /* provide seed for random number generator used to create apache pid files */
              srandom( time( NULL ) );

              /* collect the IPs, IPs and ports, and hostnames we'll need to serve */
              set< Address > unique_ip;
              set< Address > unique_ip_and_port;
              vector< pair< string, Address > > hostname_to_ip;
              set< pair< string, Address > > hostname_to_ip_set;
              map<string, Address> hostname_to_address_map;
              map< Address, string > actual_address_to_hostname;
              {
                  TemporarilyUnprivileged tu;
                  /* would be privilege escalation if we let the user read directories or open files as root */

                  const vector< string > files = list_directory_contents( directory  );
  
                  for ( const auto filename : files ) {
                      FileDescriptor fd( SystemCall( "open", open( filename.c_str(), O_RDONLY ) ) );

                      MahimahiProtobufs::RequestResponse protobuf;
                      if ( not protobuf.ParseFromFileDescriptor( fd.fd_num() ) ) {
                          throw runtime_error( filename + ": invalid HTTP request/response" );
                      }

                      const Address address( protobuf.ip(), protobuf.port() );

                      unique_ip.emplace( address.ip(), 0 );
                      unique_ip_and_port.emplace( address );

                      string hostname = HTTPRequest( protobuf.request() ).get_header_value( "Host" );
                      hostname_to_ip.emplace_back( hostname, address );
                      cout << "Request first line: " << HTTPRequest( protobuf.request() ).first_line() << " IP:port: " << address.str() << endl;
                      hostname_to_ip_set.insert( make_pair(hostname, address) );
                      hostname_to_address_map.insert( make_pair(hostname, address) );
                      actual_address_to_hostname.insert( make_pair(address, hostname) );
                  }
              }

              /* Get domains to use that adopts h2 */
              set<string> domains_adopt_h2;
              string domains_to_use_filename = "/home/vaspol/Research/MobileWebOptimization/page_load_setup/build/bin/domains_to_use/" + escaped_page;
              get_domains(domains_to_use_filename, domains_adopt_h2);
              cout << "domains: ";
              for (auto domain_iter = domains_adopt_h2.begin(); domain_iter != domains_adopt_h2.end(); domain_iter++ ) {
                cout << *domain_iter;
              }
              cout << endl;

              /* set up dummy interfaces */
              unsigned int interface_counter = 5;
              vector< pair< string, Address > > hostname_to_reverse_proxy_addresses;
              vector< pair< Address, Address > > webserver_to_reverse_proxy_addresses;
              vector< pair< string, string > > hostname_to_reverse_proxy_names;
              vector< pair< string, Address > > name_resolution_pairs;
              set< string > added_ip_addresses;

              string http_default_webserver_name = to_string( 1 ) + "default";
              string http_default_webserver_device_name = "default" + to_string( 1 );
              Address http_default_webserver_address = Address::reverse_proxy(1, 80);
              add_dummy_interface( http_default_webserver_device_name, http_default_webserver_address);
              unique_ip_and_port.emplace(http_default_webserver_address);

              name_resolution_pairs.push_back(make_pair(http_default_webserver_name, http_default_webserver_address));

              map< Address, Address > webserver_ip_to_reverse_proxy_ip;
              map< Address, string > webserver_ip_to_reverse_proxy_name;
              map< Address, string > webserver_ip_to_hostname;
              for ( set< pair< string, Address > >::iterator it = hostname_to_ip_set.begin();
                  it != hostname_to_ip_set.end(); ++it) {
                  // Get the appropriate variables.
                  auto hostname = it->first;
                  auto address = it->second;

                  // Setup the interface for each of the webserver.
                  if ( added_ip_addresses.count(address.ip()) == 0 ) {
                    add_dummy_interface( "sharded" + to_string( interface_counter ), address );
                    interface_counter++;
                  }
                  added_ip_addresses.insert( address.ip() );

                  // Check whether this IP corresponds to a reverse proxy IP or not.
                  if ( webserver_ip_to_reverse_proxy_ip.find( address ) == webserver_ip_to_reverse_proxy_ip.end() ) {
                    // There isn't an entry for the webserver ip in the map.
 
                    // Setup interfaces for reverse proxies.
                    string reverse_proxy_name = to_string( interface_counter + 1 ) + ".reverse.com";
                    string reverse_proxy_device_name = "reverse" + to_string( interface_counter + 1 );
                    Address reverse_proxy_address = Address::reverse_proxy(interface_counter + 1, address.port());
                    add_dummy_interface( reverse_proxy_device_name, reverse_proxy_address );

                    webserver_ip_to_reverse_proxy_ip[address] = reverse_proxy_address;
                    webserver_ip_to_reverse_proxy_name[address] = reverse_proxy_name;
                    interface_counter++;
                  }

                  // Populuate name resolution pairs.
                  auto reverse_proxy_address = webserver_ip_to_reverse_proxy_ip[address];
                  auto reverse_proxy_name = webserver_ip_to_reverse_proxy_name[address];
                  if (reverse_proxy_address.port() == 80) {
                    // CASE: HTTP; don't resolve the actual domain, but resolve reverse proxy to the 
                    // reverse proxy address instead. The client can now resolve the proxy.
                    name_resolution_pairs.push_back(make_pair(reverse_proxy_name, reverse_proxy_address));

                    // Add an entry for HTTPS just in case and point it to the default webserver.
                    // name_resolution_pairs.push_back(make_pair(hostname, http_default_reverse_proxy_address));
                  } else if (reverse_proxy_address.port() == 443) {
                    // CASE: HTTPS; The client will have to directly connect to the reverse proxy.
                    // The hostname of the domain should resolve directly to the reverse proxy address.
                    name_resolution_pairs.push_back(make_pair(hostname, reverse_proxy_address));
                  }

                  // Populate other information.
                  cout << "[h2] Hostname: " << hostname << " reverse proxy addr: " << reverse_proxy_address.str() << " host IP: " << address.str() << endl;
                  hostname_to_reverse_proxy_addresses.push_back(make_pair(hostname, reverse_proxy_address));
                  hostname_to_reverse_proxy_names.push_back(make_pair(hostname, reverse_proxy_name));
                  webserver_to_reverse_proxy_addresses.push_back(make_pair(address, reverse_proxy_address));
                  webserver_ip_to_hostname.insert( make_pair(address, hostname) );
              }

              string path_to_dependency_file = argv[8];

              /* set up web servers */
              vector< WebServer > servers;

              servers.emplace_back( http_default_webserver_address, working_directory, directory, escaped_page, false );
              for ( const auto ip_port : unique_ip_and_port ) {
                auto hostname = webserver_ip_to_hostname[ip_port];
                bool single_threaded = domains_adopt_h2.find(hostname) != domains_adopt_h2.end();
                cout << "Hostname: " << hostname << " ip: " << ip_port.str() << " single_threaded: " << to_string(single_threaded) << endl;
                if (path_to_dependency_file == "None") {
                  servers.emplace_back( ip_port, working_directory, directory, escaped_page, single_threaded );
                } else {
                  if ( single_threaded ) {
                    servers.emplace_back( ip_port, working_directory, directory, escaped_page, path_to_dependency_file, single_threaded);
                  } else {
                    servers.emplace_back( ip_port, working_directory, directory, escaped_page, single_threaded);
                  }
                }
              }

              /* set up nghttpx proxies */
              vector< ReverseProxy > reverse_proxies;

              for (auto webserver_to_reverse_proxy_ip_pair : webserver_to_reverse_proxy_addresses ) {
                auto webserver_address = webserver_to_reverse_proxy_ip_pair.first;
                auto reverse_proxy_address = webserver_to_reverse_proxy_ip_pair.second;
                auto hostname = webserver_ip_to_hostname[webserver_address];
                bool single_threaded = domains_adopt_h2.find(hostname) != domains_adopt_h2.end();
                reverse_proxies.emplace_back(reverse_proxy_address,
                                          webserver_address,
                                          nghttpx_path,
                                          nghttpx_key_path,
                                          nghttpx_cert_path,
                                          escaped_page,
                                          single_threaded );
              }

              PacFile pac_file("/home/vaspol/Sites/config_testing.pac");
              cout << hostname_to_reverse_proxy_addresses.size() << endl;
              // pac_file.WriteProxies(hostname_to_reverse_proxy_addresses,
              //                       hostname_to_reverse_proxy_names);
              pac_file.WriteProxies(hostname_to_reverse_proxy_addresses);

              /* set up DNS server */
              TempFile dnsmasq_hosts( "/tmp/replayshell_hosts" );
              uint8_t counter = 0;
              cout << "name_resolution_pairs.size(): " << to_string(name_resolution_pairs.size()) << endl;
              for ( const auto mapping : name_resolution_pairs ) {
                cout << "IP: " << mapping.second.ip() << " domain: " << mapping.first << endl;
                dnsmasq_hosts.write( mapping.second.ip() + " " + mapping.first + "\n" );
                counter++;
              }

              /* initialize event loop */
              EventLoop event_loop;

              /* create dummy interface for each nameserver */
              vector< Address > nameservers = all_nameservers();
              vector< string > dnsmasq_args = { "-H", dnsmasq_hosts.name() };

              for ( unsigned int server_num = 0; server_num < nameservers.size(); server_num++ ) {
                  const string interface_name = "nameserver" + to_string( server_num );
                  add_dummy_interface( interface_name, nameservers.at( server_num ) );
              }

              /* set up DNAT between tunnel and the nameserver. */
              DNATWithPostrouting dnat( Address(nameservers[0].ip(), 53), "udp", 53 );

              // Create a NAT to the first nameserver.
              /* set up NAT between egress and eth0 */
              NAT nat_rule( nameservers[0] );

              /* start dnsmasq */
              event_loop.add_child_process( start_dnsmasq( dnsmasq_args ) );

              string mode = argv[7];

              cout << "mode: " << mode << endl;

              vector< string > command;

              string path_prefix(PATH_PREFIX);
              string path_to_security_files = "/etc/openvpn/";
              VPN vpn(path_to_security_files, ingress_addr, nameservers);
              string mapping_filename = path_prefix + "/bin/webserver_to_reverse_proxy.txt";
              ofstream webserver_ip_to_reverse_proxy_mapping_file;
              webserver_ip_to_reverse_proxy_mapping_file.open(mapping_filename);
              if (mode == "regular_replay") {
                cout << "regular_replay" << endl;
                vector< string > vpn_command = vpn.start_command();
                command.insert(command.end(), vpn_command.begin(), vpn_command.end());
              } else if (mode == "per_packet_delay") {
                // Generate mapping between actual IP address and reverse proxy address.
                for ( auto it = webserver_to_reverse_proxy_addresses.begin();
                     it != webserver_to_reverse_proxy_addresses.end(); ++it ) {
                  string line = (it->first).ip() + " " + (it->second).ip();
                  webserver_ip_to_reverse_proxy_mapping_file << line << endl;
                }
                webserver_ip_to_reverse_proxy_mapping_file.close();

                cout << "per packet delay" << endl;
                command.push_back(path_prefix + "/bin/mm-delay-with-nameserver");
                command.push_back("0"); // Additional delay
                command.push_back(nameservers[0].ip());
                command.push_back(mapping_filename);
                // command.push_back("bash");
              } else {
                command.push_back("bash");
              }

              /* start shell */
              event_loop.add_child_process( join( command ), [&]() {
                      drop_privileges();

                      /* restore environment and tweak bash prompt */
                      environ = user_environment;
                      prepend_shell_prefix( "[proxy-replay] " );

                      return ezexec( command, true );
              } );

            return event_loop.loop();

          }, true); /* new network namespace */

          /* give ingress to container */
          run( { IP, "link", "set", "dev", ingress_name, "netns", to_string( container_process.pid() ) } );
          veth_devices.set_kernel_will_destroy();

          /* tell ChildProcess it's ok to proceed */
          pipe.first.write( "x" );

          /* now that we have its pid, move container process to event loop */
          outer_event_loop.add_child_process( move( container_process ) );
        }

        return outer_event_loop.loop();
    } catch ( const exception & e ) {
        print_exception( e );
        return EXIT_FAILURE;
    }
}
