/*
    This file is part of Kismet

    Kismet is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Kismet is distributed in the hope that it will be useful,
      but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Kismet; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#ifndef __Datasourcetracker_H__
#define __Datasourcetracker_H__

#include "config.hpp"

#include <pthread.h>
#include <string>
#include <vector>
#include <map>
#include <functional>

#include "globalregistry.h"
#include "util.h"
#include "kis_datasource.h"
#include "trackedelement.h"
#include "kis_net_microhttpd.h"
#include "entrytracker.h"
#include "timetracker.h"
#include "tcpserver2.h"

/* Data source tracker
 *
 * Core of the new capture management system.
 *
 * This code replaces the old packetsource tracker.
 *
 * Data sources are registered passing a builder instance which is used to
 * instantiate the final versions of the data sources.  
 *
 * Data sources communicate via the protocol defined in simple_cap_proto.h 
 * and may communicate packets or complete device objects.
 *
 * 'Auto' type sources (sources with type=auto or no type given) are 
 * probed automatically via all the registered datasource drivers.  
 * Datasource drivers may require starting a process in order to perform the
 * probe, or they may be able to perform the probe in C++ native code.
 *
 * Once a source driver is found, it is instantiated as an active source and
 * put in the list of sources.  Opening the source may result in an error, 
 * but as the source is actually assigned, it will remain in the source list.
 * This is to allow defining sources that may not be plugged in yet, etc.
 *
 * Devices which encounter errors are placed in the error vector and 
 * periodically re-tried
 *
 */

class Datasourcetracker;
class KisDatasource;
class DST_Worker;

// Worker class used to perform work on the list of packet-sources in a thread
// safe / continuity safe context.
class DST_Worker {
public:
    DST_Worker() { };

    // Handle a data source when working on iterate_datasources
    virtual void handle_datasource(shared_ptr<KisDatasource> in_src __attribute__((unused))) { };

    // All data sources have been processed in iterate_datasources
    virtual void finalize() { };
};

// Probe resolution for auto type sources
//
// Scans drivers which don't need IPC for probing first and returns immediately
// if one of them is able to handle the probe without an IPC.
// 
// Spawns IPC sources for all prototype sources concurrently.
// The first source to answer a probe with an affirmative wins; the rest of the
// probes are cancelled.
//
// After 5 seconds, probing is cancelled.
class DST_DatasourceProbe {
public:
    DST_DatasourceProbe(GlobalRegistry *in_globalreg, string in_definition, 
            SharedTrackerElement in_protovec);
    virtual ~DST_DatasourceProbe();

    void probe_sources(function<void (SharedDatasourceBuilder)> in_cb);

    string get_definition() { return definition; }

    SharedDatasourceBuilder get_proto();

    // Complete a probe - when the last one completes we're done
    void complete_probe(bool in_success, unsigned int in_transaction, string in_reason);

    void cancel();

protected:
    pthread_mutex_t probe_lock;

    GlobalRegistry *globalreg;

    shared_ptr<Timetracker> timetracker;

    // Probing instances
    map<unsigned int, SharedDatasource> ipc_probe_map;

    SharedTrackerElement proto_vec;

    // Vector of sources we're still waiting to return from probing
    vector<SharedDatasource> probe_vec;

    // Vector of sources which are complete and waiting for cleanup
    vector<SharedDatasource> complete_vec;

    // Prototype we found
    SharedDatasourceBuilder source_builder;

    // Transaction ID
    unsigned int transaction_id;

    string definition;

    function<void (SharedDatasourceBuilder)> probe_cb;
    bool cancelled;

    int cancel_timer;
};

typedef shared_ptr<DST_DatasourceProbe> SharedDSTProbe;

// List all interface supported by a phy
//
// Handles listing interfaces supported by kismet
//
// Populated with a list transaction ID, and the prototype sources, 
//
// Scans drivers which don't need IPC launching first, then launches all 
// IPC sources capable of doing an interface list and sends a query.
//
// IPC sources spawned concurrently, and results aggregated.
//
// List requests cancelled after 5 seconds
class DST_DatasourceList {
public:
    DST_DatasourceList(time_t in_time,
            shared_ptr<Datasourcetracker> in_tracker, 
            vector<SharedDatasourceBuilder> in_protovec,
            unsigned int in_transaction);
    virtual ~DST_DatasourceList();

    time_t get_time() { return start_time; }
    shared_ptr<Datasourcetracker> get_tracker() { return tracker; }

    SharedTrackerElement get_device_list() {
        return device_list;
    }

    void cancel();

protected:
    pthread_mutex_t probe_lock;

    shared_ptr<Datasourcetracker> tracker;

    SharedTrackerElement device_list;

    map<pid_t, shared_ptr<RingbufferHandler> > ipc_handler_map;

    // Vector of sources we're still waiting to return from listing
    vector<shared_ptr<KisDatasource> > listsrc_vec;

    // Source we matched
    shared_ptr<KisDatasource> protosrc;

    time_t start_time;
};

typedef shared_ptr<DST_DatasourceList> SharedDSTList;

// Tracker/serializable record of default values used for all datasources
class datasourcetracker_defaults : public tracker_component {
public:
    datasourcetracker_defaults(GlobalRegistry *in_globalreg, int in_id) :
        tracker_component(in_globalreg, in_id) {
        register_fields();
        reserve_fields(NULL);
        }

    datasourcetracker_defaults(GlobalRegistry *in_globalreg, int in_id,
            SharedTrackerElement e) :
        tracker_component(in_globalreg, in_id) {
        register_fields();
        reserve_fields(e);
    }

    virtual SharedTrackerElement clone_type() {
        return SharedTrackerElement(new datasourcetracker_defaults(globalreg, get_id()));
    }

    __Proxy(hop_rate, double, double, double, hop_rate);
    __Proxy(hop, uint8_t, bool, bool, hop);
    __Proxy(split_same_sources, uint8_t, bool, bool, split_same_sources);
    __Proxy(random_channel_order, uint8_t, bool, bool, random_channel_order);

protected:
    virtual void register_fields() {
        tracker_component::register_fields();

        RegisterField("kismet.datasourcetracker.default.hop_rate", TrackerDouble,
                "default hop rate for sources", &hop_rate);
        RegisterField("kismet.datasourcetracker.default.hop", TrackerUInt8,
                "do sources hop by default", &hop);
        RegisterField("kismet.datasourcetracker.default.split", TrackerUInt8,
                "split channels among sources with the same type", 
                &split_same_sources);
        RegisterField("kismet.datasourcetracker.default.random_order", TrackerUInt8,
                "scramble channel order to maximize use of overlap",
                &random_channel_order);
    }

    // Double hoprate per second
    SharedTrackerElement hop_rate;

    // Boolean, do we hop at all
    SharedTrackerElement hop;

    // Boolean, do we try to split channels up among the same driver?
    SharedTrackerElement split_same_sources;

    // Boolean, do we scramble the hop pattern?
    SharedTrackerElement random_channel_order;

};

class Datasourcetracker : public Kis_Net_Httpd_Stream_Handler, 
    public LifetimeGlobal, public TcpServerV2 {
public:
    static shared_ptr<Datasourcetracker> create_dst(GlobalRegistry *in_globalreg) {
        shared_ptr<Datasourcetracker> mon(new Datasourcetracker(in_globalreg));
        in_globalreg->RegisterLifetimeGlobal(mon);
        in_globalreg->InsertGlobal("DATASOURCETRACKER", mon);
        mon->datasourcetracker = mon;
        return mon;
    }

private:
    Datasourcetracker(GlobalRegistry *in_globalreg);

public:
    virtual ~Datasourcetracker();

    // Start up the system once kismet is up and running; this happens just before
    // the main select loop in kismet
    int system_startup();

    // Add a driver
    int register_datasource(SharedDatasourceBuilder in_builder);

    // Handle everything about launching a source, given a basic source line
    //
    // If there is no type defined or the type is 'auto', attempt to find the
    // driver via local probe.
    //
    // Optional completion function will be called, asynchronously,
    // on completion.
    void open_datasource(string in_source, function<void (bool, string)> in_cb);

    // Launch a source with a known prototype, given a basic source line
    // and a prototype.
    //
    // Optional completion function will be called on error or success
    void open_datasource(string in_source, SharedDatasourceBuilder in_proto,
            function<void (bool, string)> in_cb);

    // Remove a data source by UUID; stop it if necessary
    bool remove_datasource(uuid in_uud);
    // Remove a data source by index; stop it if necessary
    bool remove_datasource(int in_index);

    // HTTP api
    virtual bool Httpd_VerifyPath(const char *path, const char *method);

    virtual void Httpd_CreateStreamResponse(Kis_Net_Httpd *httpd,
            Kis_Net_Httpd_Connection *connection,
            const char *url, const char *method, const char *upload_data,
            size_t *upload_data_size, std::stringstream &stream);

    virtual int Httpd_PostIterator(void *coninfo_cls, enum MHD_ValueKind kind, 
            const char *key, const char *filename, const char *content_type,
            const char *transfer_encoding, const char *data, 
            uint64_t off, size_t size);

    // Operate on all data sources currently defined.  The datasource tracker is locked
    // during this operation, making it thread safe.
    void iterate_datasources(DST_Worker *in_worker);

    // TCPServerV2 API
    virtual void NewConnection(shared_ptr<RingbufferHandler> conn_handler);

protected:
    GlobalRegistry *globalreg;

    shared_ptr<Datasourcetracker> datasourcetracker;
    shared_ptr<EntryTracker> entrytracker;
    shared_ptr<Timetracker> timetracker;

    pthread_mutex_t dst_lock;

    SharedTrackerElement dst_proto_builder;
    SharedTrackerElement dst_source_builder;

    // Available prototypes
    SharedTrackerElement proto_vec;

    // Active data sources
    SharedTrackerElement datasource_vec;

    // Sub-workers probing for a source definition
    map<unsigned int, SharedDSTProbe> probing_map;
    unsigned int next_probe_id;

    // Sub-workers slated for being removed
    vector<SharedDSTProbe> probing_complete_vec;

    // Sub-workers listing interfaces
    map<unsigned int, SharedDSTList> listing_map;
    unsigned int next_list_id;

    // Sub-workers slated for being removed
    vector<SharedDSTList> listing_complete_vec;

    // Cleanup task
    int completion_cleanup_id;
    void schedule_cleanup();

    // UUIDs to source numbers
    unsigned int next_source_num;
    map<uuid, unsigned int> uuid_source_num_map;

    shared_ptr<datasourcetracker_defaults> config_defaults;

    // Re-assign channel hopping because we've opened a new source
    // and want to do channel split
    void calculate_source_hopping(SharedDatasource in_ds);

};


#endif

