/**
Copyright 2009-2017 National Technology and Engineering Solutions of Sandia,
LLC (NTESS).  Under the terms of Contract DE-NA-0003525, the U.S.  Government
retains certain rights in this software.

Sandia National Laboratories is a multimission laboratory managed and operated
by National Technology and Engineering Solutions of Sandia, LLC., a wholly
owned subsidiary of Honeywell International, Inc., for the U.S. Department of
Energy's National Nuclear Security Administration under contract DE-NA0003525.

Copyright (c) 2009-2017, NTESS

All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.

    * Redistributions in binary form must reproduce the above
      copyright notice, this list of conditions and the following
      disclaimer in the documentation and/or other materials provided
      with the distribution.

    * Neither the name of Sandia Corporation nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

Questions? Contact sst-macro-help@sandia.gov
*/

#include <dumpi/libotf2dump/otf2writer.h>
#include <sys/stat.h>
#include <algorithm>
#include <limits.h>
#include <cstring>
#include <unordered_set>
#include <functional>
#include <iostream>


template <class T>
static T array_sum(T* t, int n){
  typename std::decay<T>::type sum = 0;
  for (int i=0; i < n; ++i){
    sum += t[i];
  }
  return sum;
}




#define _ENTER(name)                         \
  logger(OWV_INFO, name);                    \
  start_time_ = std::min(start_time_, start); \
  stop_time_ = std::max(stop_time_, stop);    \
  auto this_region = otf2_regions_table_.insert(name);  \
  OTF2_EvtWriter_Enter(evt_writer,      \
                       nullptr,             \
                       start,               \
                       this_region);        \
  event_count_++;

#define _LEAVE()                         \
  OTF2_EvtWriter_Leave(evt_writer,   \
                       nullptr,          \
                       stop,             \
                       this_region);     \
  event_count_++;                \
  return OTF2_WRITER_SUCCESS;

#define COLLECTIVE_WRAPPER(collective, sent, received)                                                    \
  OTF2_EvtWriter_MpiCollectiveBegin(evt_writer, nullptr, start);                                      \
  OTF2_EvtWriter_MpiCollectiveEnd(evt_writer, nullptr, stop, collective, comm, root, sent, received); \
  event_count_ += 2;

static OTF2_CallbackCode
otf2writer_get_rank_cb(void* userData,
                       OTF2_CollectiveContext* commContext,
                       uint32_t* rank)
{
  auto data = (dumpi::OTF2_Writer::worldConfig*) userData;
  *rank = data->rank;
  return OTF2_CALLBACK_SUCCESS;
}

static OTF2_CallbackCode
otf2writer_get_size_cb(void* userData,
                       OTF2_CollectiveContext* commContext,
                       uint32_t* size )
{

  auto data = (dumpi::OTF2_Writer::worldConfig*) userData;
  *size = data->size;
  return OTF2_CALLBACK_SUCCESS;
}

static OTF2_CallbackCode
otf2writer_barrier_cb(void* userData,
                      OTF2_CollectiveContext* commContext )
{
  return OTF2_CALLBACK_SUCCESS;
}


static OTF2_CallbackCode
otf2writer_bcast_cb(void*                   userData,
                    OTF2_CollectiveContext* commContext,
                    void*                   data,
                    uint32_t                numberElements,
                    OTF2_Type               type,
                    uint32_t                root)
{
  return OTF2_CALLBACK_SUCCESS;
}


static OTF2_CallbackCode
otf2writer_gather_cb(void*                   userData,
                     OTF2_CollectiveContext* commContext,
                     const void*             inData,
                     void*                   outData,
                     uint32_t                numberElements,
                     OTF2_Type               type,
                     uint32_t                root)
{
  return OTF2_CALLBACK_SUCCESS;
}


static OTF2_CallbackCode
otf2writer_gatherv_cb(void*                   userData,
                     OTF2_CollectiveContext* commContext,
                     const void*             inData,
                     uint32_t                inElements,
                     void*                   outData,
                     const uint32_t*         outElements,
                     OTF2_Type               type,
                     uint32_t                root)
{
  return OTF2_CALLBACK_SUCCESS;
}


static OTF2_CallbackCode
otf2writer_scatter_cb(void*                   userData,
                      OTF2_CollectiveContext* commContext,
                      const void*             inData,
                      void*                   outData,
                      uint32_t                numberElements,
                      OTF2_Type               type,
                      uint32_t                root)
{
  return OTF2_CALLBACK_SUCCESS;
}


static OTF2_CallbackCode
otf2writer_scatterv_cb(void*                   userData,
                       OTF2_CollectiveContext* commContext,
                       const void*             inData,
                       const uint32_t*         inElements,
                       void*                   outData,
                       uint32_t                outElements,
                       OTF2_Type               type,
                       uint32_t                root)
{
  return OTF2_CALLBACK_SUCCESS;
}

static const OTF2_CollectiveCallbacks dumpi_otf2writer_collectives =
{
  NULL,
  otf2writer_get_size_cb,
  otf2writer_get_rank_cb,
  NULL,
  NULL,
  otf2writer_barrier_cb,
  otf2writer_bcast_cb,
  otf2writer_gather_cb,
  otf2writer_gatherv_cb,
  otf2writer_scatter_cb,
  otf2writer_scatterv_cb
};

namespace dumpi {

OTF2_Writer::OTF2_Writer() :
  archive_(nullptr),
  start_time_(~0),
  stop_time_(0),
  comm_world_id_(-1),
  comm_self_id_(-1),
  comm_error_id_(-1),
  comm_null_id_(-1),
  event_count_(0),
  verbosity_(OWV_NONE),
  clock_resolution_(1)
{
}


// OTF2 callbacks
static OTF2_FlushType pre_flush( void* userData, OTF2_FileType fileType,
                                 OTF2_LocationRef location, void* callerData, bool final) {
  return OTF2_FLUSH;
}
static OTF2_TimeStamp post_flush(void* userData, OTF2_FileType fileType, OTF2_LocationRef location) {
  return 0;
}

OTF2_FlushCallbacks flush_callbacks =
{
  pre_flush,
  post_flush
};

OTF2_WRITER_RESULT
OTF2_Writer::open_archive(const std::string& path, int size, int rank)
{
  if (archive_ != nullptr)
    return OTF2_WRITER_ERROR_ARCHIVE_ALREADY_OPEN;

  struct stat sb;

  // test if directory already exists
  // if (stat(path.c_str(), &sb) == 0 )
  //     return OTF2_WRITER_ERROR_DIRECTORY_ALREADY_EXISTS;

  mk_archive_dir(path.c_str());

  // test if parent directory exists
  //if (stat((path + "/..").c_str(), &sb) && !force)
  //  return OTF2_WRITER_ERROR_DIRECTORY_ALREADY_EXISTS;

  world_.size = size;
  world_.rank = rank;
  directory_ = path;
  archive_ = OTF2_Archive_Open(  directory_.c_str(),
                                 "traces",
                                 OTF2_FILEMODE_WRITE,
                                 1024 * 1024 /* event chunk size */,
                                 4 * 1024 * 1024 /* def chunk size */,
                                 OTF2_SUBSTRATE_POSIX,
                                 OTF2_COMPRESSION_NONE );

  OTF2_Archive_SetCollectiveCallbacks(archive_, &dumpi_otf2writer_collectives, &world_, NULL, NULL);
  //OTF2_Archive_SetSerialCollectiveCallbacks(_archive);
  OTF2_Archive_SetFlushCallbacks( archive_, &flush_callbacks, NULL );
  OTF2_Archive_OpenEvtFiles(archive_);

  evt_writer = OTF2_Archive_GetEvtWriter(archive_, rank);

  return OTF2_WRITER_SUCCESS;
}

int
OTF2_Writer::mk_archive_dir(const char *path)
{
  const size_t len = strlen(path);
  char _path[PATH_MAX];
  char *p;

  errno = 0;

  if (len > sizeof(_path)-1) {
    errno = ENAMETOOLONG;
    return -1;
  }
  strcpy(_path, path);

  for (p = _path + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';

      if (mkdir(_path, S_IRWXU) != 0) {
        if (errno != EEXIST)
          return -1;
      }
      *p = '/';
    }
  }

  return (mkdir(_path, S_IRWXU) != 0 && errno != EEXIST) ? -1 : 0;
}

// lousy logger implementation
void
OTF2_Writer::logger(OTF2_WRITER_VERBOSITY level, const std::string& msg)
{
  logger(level, msg.c_str());
}

void
OTF2_Writer::logger(OTF2_WRITER_VERBOSITY level, const char* msg)
{
  if (verbosity_ >= level && verbosity_ != OWV_NONE) {
    const char* prepender = "";
    switch (level) {
      case OWV_ABORT:
      prepender = "OTF2 WRITER (ABRT): ";
      break;
      case OWV_ERROR:
      prepender = "OTF2 WRITER  (ERR): ";
      break;
      case OWV_WARN:
      prepender = "OTF2 WRITER  (WRN): ";
      break;
      case OWV_INFO:
      prepender = "OTF2 WRITER (INFO): ";
      break;
      default:
      prepender = "OTF2 WRITER  (UNK): ";
      break;
    }
    printf("%s%s\n", prepender, msg);
    fflush(stdout);
  }
}

void
OTF2_Writer::check_otf2(OTF2_ErrorCode status, const char* description)
{
  if ( status != OTF2_SUCCESS ) {
    auto msg = std::string("OTF2 Error: ");
    msg += description;
    msg += "\n\t ";
    msg += OTF2_Error_GetDescription(status);
    logger(OWV_ERROR, msg);
  }
}

/**
void
OTF2_Writer::write_local_def_files()
{
  OTF2_DefWriter* local_def_writer = OTF2_Archive_GetDefWriter(archive_, world_.rank);
  // initialize local COMM mappings table, include space for MPI_COMM_WORLD, MPI_COMM_SELF, and any user-defined communicators.
  OTF2_IdMap* mpi_comm_map = OTF2_IdMap_Create(OTF2_ID_MAP_SPARSE, context_.comm_mapping.size());
  for (auto& pair : builder.local_to_global_comm_id){
    check_otf2(OTF2_IdMap_AddIdPair(mpi_comm_map, pair.first, pair.second),
               "Adding a communicator to the Def mapping list");
  }
  //write the table to a local def file
  check_otf2(OTF2_DefWriter_WriteMappingTable( local_def_writer,
                                    OTF2_MAPPING_COMM,
                                    mpi_comm_map ),
                     "Writing a communicator mapping table to the OTF2 archive");
  check_otf2(OTF2_Archive_CloseDefWriter(archive_, local_def_writer), "Closing a local def writer");
  OTF2_IdMap_Free(mpi_comm_map);
}
*/

void
OTF2_Writer::write_def_files(const std::vector<int>& event_counts)
{
  // OTF2 Reference Example
  // https://silc.zih.tu-dresden.de/otf2-current/group__usage__writing__mpi.htm

  // See ScoreP source for a local def writer in action
  //  vendor/otf2/src/tools/otf2_trace_gen/otf2_trace_gen.c

  check_otf2(OTF2_Archive_OpenDefFiles(archive_), "Opening definition files");
  OTF2_GlobalDefWriter* defwriter = OTF2_Archive_GetGlobalDefWriter(archive_);


  if (clock_resolution_ == 0) logger(OWV_ERROR, "Clock Resolution not set, use 'set_clock_resolution() to set ticks per second'");

  check_otf2(OTF2_GlobalDefWriter_WriteClockProperties( defwriter,
                                                        clock_resolution_,
                                                        start_time_,
                                                        stop_time_ - start_time_), "Writing clock properties to global def file");

  // Strings must come first in the def file
  // Otherwise tools like otf2_print will report errors, even though all of the information is available
  int EMPTY_STRING = otf2_strings_table_.insert("");
  otf2_strings_table_.insert("MPI");
  otf2_strings_table_.insert("MPI_COMM_WORLD");
  otf2_strings_table_.insert("MPI_COMM_SELF");
  otf2_strings_table_.insert("LOCATIONS_GROUP");

  for(int rank = 0; rank < world_.size; rank++) {
    otf2_strings_table_.insert(std::string("MPI Rank ") + std::to_string(rank));
    otf2_strings_table_.insert(std::string("Master Thread ") + std::to_string(rank));
  }

  for(int i = 0; i < otf2_regions_table_.size(); i++){
    otf2_strings_table_.insert(otf2_regions_table_.get(i));
  }

  /**
  TODO
  for(auto comm_it = mpi_comm_.begin(); comm_it != mpi_comm_.end(); comm_it++){
    char comm_name[32];
    sprintf(comm_name, "MPI Comm %d", i);
    otf2_strings_table_.insert(comm_name);
  }
  */

  // STRINGS
  logger(OWV_INFO, "Writing STRINGs to the def file");
  for(auto& pair : otf2_strings_table_) {
    int idx = pair.second;
    auto& name = pair.first;
    check_otf2(OTF2_GlobalDefWriter_WriteString(defwriter, idx, name.c_str()),
               "Writing string to global def file");
  }

  //PARADIGM
  logger(OWV_INFO, "Writing PARADIGMs to the def file");
  check_otf2(OTF2_GlobalDefWriter_WriteParadigm( defwriter,
                                      OTF2_PARADIGM_MPI,
                                      otf2_strings_table_.insert("MPI"),
                                      OTF2_PARADIGM_CLASS_PROCESS),
                                      "Writing Paradigm to global def file");

  // REGIONS
  logger(OWV_INFO, "Writing REGIONs to the def file");
  for(auto& pair : otf2_regions_table_) {
    int region_idx = pair.second;
    auto& region_name = pair.first;
    int str_id = otf2_strings_table_.get(region_name);
    check_otf2(OTF2_GlobalDefWriter_WriteRegion(defwriter,
                                     region_idx,                   /* id */
                                     str_id,                        /* region name  */
                                     str_id,                        /* alternative name */
                                     0,                             /* description */
                                     OTF2_REGION_ROLE_FUNCTION,
                                     OTF2_PARADIGM_MPI,
                                     OTF2_REGION_FLAG_NONE,
                                     0,                             /* source file */
                                     0,                             /* begin lno */
                                     0                              /* end lno */
               ),
               "Writing Region to global def file");
  }

  // SYSTEM_TREE_NODE
  logger(OWV_INFO, "Writing SYSTEM_TREE_NODE to the def file");
  check_otf2(OTF2_GlobalDefWriter_WriteSystemTreeNode( defwriter,
                                            0 /* id */,
                                            0 /* name */,
                                            0 /* class */,
                                            OTF2_UNDEFINED_SYSTEM_TREE_NODE /* parent */ ),
                                            "Writing SystemTreeNode to global def file");

  // LOCATION_GROUP
  logger(OWV_INFO, "Writing LOCATION_GROUPs to the def file");
  // locationgroup appears to be used to identify computation localities. For example, all of the threads on a given node.
  for (int i = 0; i < world_.size; i++) {
    char rank_name[32];
    sprintf(rank_name, "MPI Rank %d", i);
    check_otf2(OTF2_GlobalDefWriter_WriteLocationGroup(defwriter,
                                            i,
                                            otf2_strings_table_.insert(rank_name),
                                            OTF2_LOCATION_GROUP_TYPE_PROCESS,
                                            0 /* This should point to the node this rank ran on. Not necessary for sst/macro trace replay.*/),
                                            "Writing Location Group to global def file");
  }

  // LOCATION
  logger(OWV_INFO, "Writing LOCATIONs to the def file");
  // Each location will identifies a rank. Must come before groups.
  for (int i = 0; i < world_.size; i++) {
    char loc_name[32];
    sprintf(loc_name, "Master Thread %d", i);
    check_otf2(OTF2_GlobalDefWriter_WriteLocation(defwriter,
                                       i,
                                       otf2_strings_table_.insert(loc_name),
                                       OTF2_LOCATION_TYPE_CPU_THREAD,
                                       event_counts[i],
                                       0),
                                       "Writing Location to global def file");
  }

  // Here we emulate ScoreP's behavior: create a common preamble to every trace, and remap communicators as necesary
  //    GROUP                                0  Name: "" <0>, Type: COMM_LOCATIONS, Paradigm: "MPI" <4>, Flags: NONE, 4 Members: "Master thread" <0>, "Master thread" <1>, "Master thread" <2>, "Master thread" <3>
  //    GROUP                                1  Name: "" <0>, Type: COMM_GROUP, Paradigm: "MPI" <4>, Flags: NONE, 4 Members: 0 ("Master thread" <0>), 1 ("Master thread" <1>), 2 ("Master thread" <2>), 3 ("Master thread" <3>)
  //    GROUP                                2  Name: "" <0>, Type: COMM_SELF, Paradigm: "MPI" <4>, Flags: NONE, 0 Members:
  //    COMM                                 0  Name: "MPI_COMM_WORLD" <329>, Group: "" <1>, Parent: UNDEFINED
  //    COMM                                 1  Name: "MPI_COMM_SELF" <334>, Group: "" <2>, Parent: UNDEFINED

  // GROUP
  logger(OWV_INFO, "Writing GROUPs to the def file");

  // ScoreP behavior: Each COMM points to a GROUP def of type OTF2_GROUP_TYPE_COMM_GROUP.
  // Althout the GROUP defs enumerate their ranks, Each must point to a root group of type OTF2_GROUP_TYPE_COMM_LOCATIONS.
  // ScoreP does not seem to nest groups, instead flattening each to the global LOCATIONS group that points directly to the COMM_LOCATIONS group.
  uint64_t* world_list = new uint64_t[world_.size];
  for (int i = 0; i < world_.size; i++)  world_list[i] = i;

  check_otf2(OTF2_GlobalDefWriter_WriteGroup(defwriter,
                                  COMM_LOCATIONS_GROUP_ID,
                                  otf2_strings_table_.insert("LOCATIONS_GROUP"),
                                  OTF2_GROUP_TYPE_COMM_LOCATIONS,
                                  OTF2_PARADIGM_MPI,
                                  OTF2_GROUP_FLAG_NONE,
                                  world_.size,
                                  world_list),
                                  "Writing Locations Group to global def file");
  delete[] world_list;

  // The runtime will create duplicate comm groups, only use the ones referenced by communicators

  // Having non-contiguous group (integer) ID's makes otf2-print dump warnings. "otf2-print: warning: out of order Comm definition"
  // At this point all of the comm's and groups have been realized, and a last second remapping will solve the above warning
  std::map<int, int> group_id_map;
  int group_map_inc = 1;

  //for(auto group_it = _mpi_group.begin(); group_it != _mpi_group.end(); group_it++) {
  /**
  for(auto comm_it = mpi_comm_.begin(); comm_it != mpi_comm_.end(); comm_it++) {
    auto group_id = comm_it->second.group;
    auto& members = mpi_group_[group_id];
    group_id_map[group_id] = group_map_inc;
    uint64_t* group_list = new uint64_t[members.size()];
    for (int i = 0; i < members.size(); i++)  group_list[i] = (uint64_t)members[i];

    check_otf2(OTF2_GlobalDefWriter_WriteGroup(defwriter,
                                    //group_it->first + USER_DEF_COMM_GROUP_OFFSET,
                                    group_map_inc++,
                                    EMPTY_STRING,
                                    OTF2_GROUP_TYPE_COMM_GROUP,
                                    OTF2_PARADIGM_MPI,
                                    OTF2_GROUP_FLAG_NONE,
                                    members.size(),
                                    group_list),
                                    "Writing group to global def file");

    delete [] group_list;
  }
  */

  // COMM
  logger(OWV_INFO, "Writing COMMs to the def file");

  if (comm_world_id_ == -1)
    logger(OWV_ERROR, "register_comm_world() not called");

  if (comm_self_id_ == -1)
    logger(OWV_ERROR, "register_comm_self() not called");

  if (comm_null_id_ == -1)
    logger(OWV_ERROR, "register_comm_null() not called");

  /**
  TODO
  for(auto comm_it = mpi_comm_.begin(); comm_it != mpi_comm_.end(); comm_it++) {
    int comm_id = comm_it->first;
    MPI_Comm_Struct& comm = comm_it->second;

    check_otf2(OTF2_GlobalDefWriter_WriteComm( defwriter,
                                    comm_id,
                                    string_[comm.name],
                                    group_id_map[comm.group],
                                    comm.parent),
                                    "Writing a Communicator to global def file");
  }
  */

  check_otf2(OTF2_Archive_CloseDefFiles(archive_), "Closing definition files");
}

OTF2_WRITER_RESULT
OTF2_Writer::close_archive()
{
  check_otf2(OTF2_Archive_CloseEvtFiles(archive_), "Closing all event files");
  check_otf2(OTF2_Archive_Close(archive_), "Closing archive");

  archive_ = nullptr;

  return OTF2_WRITER_SUCCESS;
}

void
OTF2_Writer::register_comm_world(comm_t id)
{
  OTF2_MPI_Group& grp = groups_[COMM_WORLD_GROUP_ID];
  grp.is_comm_world = true;
  grp.local_id = COMM_WORLD_GROUP_ID;

  OTF2_MPI_Comm& comm = comms_[MPI_COMM_WORLD_ID];
  comm.local_id=MPI_COMM_WORLD_ID;
  comm.global_id=MPI_COMM_WORLD_ID;
  comm.local_group_id=(int)COMM_WORLD_GROUP_ID;
  comm.name="MPI_COMM_WORLD";
}

void OTF2_Writer::register_comm_self(comm_t id) {
  comm_self_id_ = id;

  auto& comm = comms_[MPI_COMM_SELF_ID];
  comm.local_id=MPI_COMM_SELF_ID;
  comm.global_id=MPI_COMM_SELF_ID;
  comm.local_group_id=(int)COMM_SELF_GROUP_ID;
  comm.name="MPI_COMM_SELF";
}

void OTF2_Writer::register_comm_error(comm_t id) {
  comm_error_id_ = id;
}

void OTF2_Writer::register_comm_null(comm_t id) {
  comm_null_id_ = id;
}

void OTF2_Writer::register_null_request(request_t request) {
  null_request_ = request;
}

void OTF2_Writer::set_verbosity(OTF2_WRITER_VERBOSITY verbosity) {
  verbosity_ = verbosity;
}

void OTF2_Writer::set_clock_resolution(uint64_t ticks_per_second) {
  clock_resolution_ = ticks_per_second;
}

// TODO type sizes should be rank specific.
void OTF2_Writer::register_type(mpi_type_t type, int size) {
  type_sizes_[type] = size;
}

OTF2_WRITER_RESULT OTF2_Writer::mpi_send_inner(otf2_time_t start, mpi_type_t type, uint64_t count,
                                               uint32_t dest, int comm, uint32_t tag) {
  OTF2_EvtWriter_MpiSend(evt_writer, nullptr, start, dest, comm, tag, type_sizes_[type]*count);
  event_count_++;
  return OTF2_WRITER_SUCCESS;
}

OTF2_WRITER_RESULT OTF2_Writer::mpi_send(otf2_time_t start, otf2_time_t stop, mpi_type_t type,
                                         uint64_t count, uint32_t dest, int comm, uint32_t tag) {
  _ENTER("MPI_Send");
  mpi_send_inner(start, type, count, dest, comm, tag);
  _LEAVE();
}

OTF2_WRITER_RESULT OTF2_Writer::mpi_bsend(otf2_time_t start, otf2_time_t stop, mpi_type_t type,
                                          uint64_t count, uint32_t dest, int comm, uint32_t tag) {
  _ENTER("MPI_Bsend");
  mpi_send_inner(start, type, count, dest, comm, tag);
  _LEAVE();
}

OTF2_WRITER_RESULT OTF2_Writer::mpi_ssend(otf2_time_t start, otf2_time_t stop,
                                          mpi_type_t type, uint64_t count, uint32_t dest,
                                          int comm, uint32_t tag) {
  _ENTER("MPI_Ssend");
  mpi_send_inner(start, type, count, dest, comm, tag);
  _LEAVE();
}

OTF2_WRITER_RESULT OTF2_Writer::mpi_rsend(otf2_time_t start, otf2_time_t stop, mpi_type_t type,
                                          uint64_t count, uint32_t dest, int comm, uint32_t tag) {
  _ENTER("MPI_Rsend");
  mpi_send_inner(start, type, count, dest, comm, tag);
  _LEAVE();
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_recv(otf2_time_t start, otf2_time_t stop, mpi_type_t type, uint64_t count,
                      uint32_t source, int comm, uint32_t tag)
{
  _ENTER("MPI_Recv");
  OTF2_EvtWriter_MpiRecv(evt_writer, nullptr, start, source, comm, tag, count_bytes(type, count));
  event_count_++;
  _LEAVE();
}

// I-event handling
void
OTF2_Writer::incomplete_call(request_t request_id, REQUEST_TYPE type) {
  if (request_id != null_request_){
    request_type[request_id] = type;
  }
}

/*
 * ISends and IRecvs begin when invoked, but important information about
 * them are not recorded by OTF2 until they are completed, which occurs inside of Waits, Tests, etc.
 * Returns the number of events generated
 */
void
OTF2_Writer::complete_call(request_t request_id, uint64_t timestamp) {
  auto iter = request_type.find(request_id);
  if (iter == request_type.end()) {
    if (request_id != null_request_){
      std::cerr << "Error: request (" << request_id << ") not found"
                << " on rank " << world_.rank << std::endl;
      abort();
    } else {
      return;
    }
  }

  switch (iter->second){
    case REQUEST_TYPE_ISEND:
      OTF2_EvtWriter_MpiIsendComplete(evt_writer, nullptr, timestamp, request_id);
      event_count_++;
      break;
    case REQUEST_TYPE_IRECV: {
      auto irecv_it = irecv_requests.find(request_id);
      irecv_capture irecv = irecv_it->second;
      if (irecv_it == irecv_requests.end()) {
        std::cerr << "Error: request id (" << request_id << ") not found"
                  << " while trying to complete MPI_IRecv" << std::endl;
        abort();
      } else {
        OTF2_EvtWriter_MpiIrecv(evt_writer, nullptr, timestamp, irecv.source, irecv.comm,
                                irecv.tag, irecv.bytes_sent, request_id);
        irecv_requests.erase(irecv_it);
        event_count_++;
      }
    }
  }
  request_type.erase(iter);
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_isend_inner(otf2_time_t start, mpi_type_t type, uint64_t count,
                             uint32_t dest, int comm, uint32_t tag, request_t request)
{
  incomplete_call(request, REQUEST_TYPE_ISEND);
  OTF2_EvtWriter_MpiIsend(evt_writer, nullptr, start, dest, comm, tag, count_bytes(type, count), request);
  event_count_++;
  return OTF2_WRITER_SUCCESS;
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_isend(otf2_time_t start, otf2_time_t stop, mpi_type_t type, uint64_t count,
                       uint32_t dest, int comm, uint32_t tag, request_t request)
{
  _ENTER("MPI_Isend");
  mpi_isend_inner(start, type, count, dest, comm, tag, request);
  _LEAVE();
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_ibsend(otf2_time_t start, otf2_time_t stop, mpi_type_t type, uint64_t count,
                        uint32_t dest, int comm, uint32_t tag, request_t request)
{
  _ENTER("MPI_Ibsend");
  mpi_isend_inner(start, type, count, dest, comm, tag, request);
  _LEAVE();
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_issend(otf2_time_t start, otf2_time_t stop, mpi_type_t type, uint64_t count,
                        uint32_t dest, int comm, uint32_t tag, request_t request)
{
  _ENTER("MPI_Issend");
  mpi_isend_inner(start, type, count, dest, comm, tag, request);
  _LEAVE();
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_irsend(otf2_time_t start, otf2_time_t stop, mpi_type_t type, uint64_t count,
                        uint32_t dest, int comm, uint32_t tag, request_t request)
{
  _ENTER("MPI_Irsend");
  mpi_isend_inner(start, type, count, dest, comm, tag, request);
  _LEAVE();
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_irecv(otf2_time_t start, otf2_time_t stop, mpi_type_t type, uint64_t count,
                       uint32_t source, int comm, uint32_t tag, request_t request)
{
  _ENTER("MPI_Irecv");
  irecv_requests[request] = {count_bytes(type, count), source, tag, comm, request};
  incomplete_call(request, REQUEST_TYPE_IRECV);
  OTF2_EvtWriter_MpiIrecvRequest(evt_writer, nullptr, start, request);
  event_count_++;
  _LEAVE();
}

OTF2_WRITER_RESULT
OTF2_Writer::generic_call(otf2_time_t start, otf2_time_t stop, const std::string& name)
{
  _ENTER(name);
  _LEAVE();
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_wait(otf2_time_t start, otf2_time_t stop, request_t request)
{
  _ENTER("MPI_Wait");
  complete_call(request, start);
  _LEAVE();
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_waitany(otf2_time_t start, otf2_time_t stop, request_t request)
{
  _ENTER("MPI_Waitany");
  complete_call(request, start);
  _LEAVE();
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_waitall(otf2_time_t start, otf2_time_t stop,
                         int count, const request_t* requests)
{
  _ENTER("MPI_Waitall");

  std::unordered_set<request_t> called;
  for(int i = 0; i < count; i++) {
    auto req = requests[i];
    if (req != null_request_ && called.find(req) == called.end()) {
      complete_call(req, start);
      called.insert(req);
    }
  }
  _LEAVE();
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_waitsome(otf2_time_t start, otf2_time_t stop, const request_t* requests,
                          int outcount, const int* indices)
{
  _ENTER("MPI_Waitsome");
  for (int i = 0; i < outcount; i++){
    complete_call(requests[indices[i]], start);
  }
  _LEAVE();
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_test(otf2_time_t start, otf2_time_t stop, request_t request, int flag)
{
  _ENTER("MPI_Test");
  if (flag){
    complete_call(request, start);
  }
  _LEAVE();
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_testany(otf2_time_t start, otf2_time_t stop, const request_t* requests,
                         int index, int flag)
{
  _ENTER("MPI_Testany");
  if (flag){
    complete_call(requests[index], start);
  }
  _LEAVE();
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_testall(otf2_time_t start, otf2_time_t stop, int count,
                         const request_t* requests, int flag)
{
  _ENTER("MPI_Testall");
  if (flag){
    for (int i = 0; i < count; i++){
      complete_call(requests[i], start);
    }
  }
  _LEAVE();
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_testsome(otf2_time_t start, otf2_time_t stop,
                          const request_t* requests, int outcount, const int* indices)
{
  _ENTER("MPI_Testsome");
  for (int i = 0; i < outcount; i++){
    complete_call(requests[indices[i]], start);
  }
  _LEAVE();
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_barrier(otf2_time_t start, otf2_time_t stop, comm_t comm)
{
  _ENTER("MPI_Barrier");
  int root = undefined_root;
  COLLECTIVE_WRAPPER(OTF2_COLLECTIVE_OP_BARRIER, 0, 0);
  _LEAVE();
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_bcast(otf2_time_t start, otf2_time_t stop, int count, mpi_type_t type,
                       int root, comm_t comm)
{
  _ENTER("MPI_Bcast");

  int world_root = get_world_rank(root, comm);
  bool is_root = world_root == world_.rank;
  int bytes = count_bytes(type, count);

  COLLECTIVE_WRAPPER(OTF2_COLLECTIVE_OP_BCAST,
                     is_root ? bytes * get_comm_size(comm) : 0, // scorep multiplies the root's send by comm size
                     bytes);
  _LEAVE();
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_gather(otf2_time_t start, otf2_time_t stop, int sendcount, mpi_type_t sendtype,
                        int recvcount, mpi_type_t recvtype, int root, comm_t comm)
{
  _ENTER("MPI_Gather");

  int world_root = get_world_rank(root, comm);
  bool is_root = world_root == world_.rank;

  COLLECTIVE_WRAPPER(OTF2_COLLECTIVE_OP_GATHER, //scorep multiplies the root receive by comm size
                     count_bytes(sendtype, sendcount),
                     (is_root ? count_bytes(recvtype, recvcount) * get_comm_size(comm) : 0));
  _LEAVE();
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_gatherv(otf2_time_t start, otf2_time_t stop, int comm_size, int sendcount,
                         mpi_type_t sendtype, const int* recvcounts, mpi_type_t recvtype,
                         int root, comm_t comm)
{
  _ENTER("MPI_Gatherv");

  int world_root = get_world_rank(root, comm);
  bool is_root = world_root == world_.rank;

  COLLECTIVE_WRAPPER(OTF2_COLLECTIVE_OP_GATHERV,
                     count_bytes(sendtype, sendcount),
                     (is_root ? count_bytes(recvtype, array_sum(recvcounts, comm_size)) : 0));
  _LEAVE();
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_scatter(otf2_time_t start, otf2_time_t stop, int sendcount, mpi_type_t sendtype,
                         int recvcount, mpi_type_t recvtype, int root, comm_t comm)
{
  _ENTER("MPI_Scatter");

  int world_root = get_world_rank(root, comm);
  bool is_root = world_root == world_.rank;

  COLLECTIVE_WRAPPER(OTF2_COLLECTIVE_OP_SCATTER,
                     // scorep multiplies the root rank's send by commm size
                     is_root ? count_bytes(sendtype, sendcount) * get_comm_size(comm) : 0,
                     count_bytes(recvtype, recvcount));
  _LEAVE();
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_scatterv(otf2_time_t start, otf2_time_t stop, int comm_size,
                          const int* sendcounts, mpi_type_t sendtype,
                          int recvcount, mpi_type_t recvtype, int root, comm_t comm)
{
  _ENTER("MPI_Scatterv");

  int world_root = get_world_rank(root, comm);
  bool is_root = world_root == world_.rank;
  COLLECTIVE_WRAPPER(OTF2_COLLECTIVE_OP_SCATTERV,
                     is_root ? count_bytes(sendtype, array_sum(sendcounts, comm_size)) : 0,
                     count_bytes(recvtype, recvcount));
  _LEAVE();
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_reduce(otf2_time_t start, otf2_time_t stop, int count,
                        mpi_type_t type, int root, comm_t comm)
{
  _ENTER("MPI_Reduce");

  int world_root = get_world_rank(root, comm);
  bool is_root = world_root == world_.rank;
  int sent = count_bytes(type, count);
  COLLECTIVE_WRAPPER(OTF2_COLLECTIVE_OP_REDUCE,
                     sent,
                     is_root ? sent * get_comm_size(comm) : 0);
  _LEAVE();
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_scan(otf2_time_t start, otf2_time_t stop, int count,
                      mpi_type_t datatype, comm_t comm)
{
  _ENTER("MPI_Scan");
  int comm_rank = get_comm_rank(comm, world_.rank);
  int bytes = count_bytes(datatype, count);
  int root = undefined_root;
  COLLECTIVE_WRAPPER(OTF2_COLLECTIVE_OP_SCAN,
                     (get_comm_size(comm) - comm_rank - 1) * bytes,
                     (comm_rank + 1) * bytes);
  _LEAVE();
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_allgather(otf2_time_t start, otf2_time_t stop, int sendcount, mpi_type_t sendtype,
                           int recvcount, mpi_type_t recvtype, comm_t comm)
{
  _ENTER("MPI_Allgather");
  int comm_size =  get_comm_size(comm);
  int root = undefined_root;
  COLLECTIVE_WRAPPER(OTF2_COLLECTIVE_OP_ALLGATHER,
                     comm_size * count_bytes(sendtype, sendcount),
                     comm_size * count_bytes(recvtype, recvcount));
  _LEAVE();
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_allgatherv(otf2_time_t start, otf2_time_t stop, int comm_size,
                            int sendcount, mpi_type_t sendtype,
                            const int* recvcounts, mpi_type_t recvtype, comm_t comm)
{
  _ENTER("MPI_Allgatherv");
  int root = undefined_root;
  COLLECTIVE_WRAPPER(OTF2_COLLECTIVE_OP_ALLGATHERV,
                     comm_size * count_bytes(sendtype, sendcount),
                     count_bytes(recvtype, array_sum(recvcounts, comm_size)));
  _LEAVE();
}
OTF2_WRITER_RESULT
OTF2_Writer::mpi_alltoall(otf2_time_t start, otf2_time_t stop,
                          int sendcount, mpi_type_t sendtype, int recvcount,
                          mpi_type_t recvtype, comm_t comm)
{
  _ENTER("MPI_Alltoall");
  int root = undefined_root;
  int transmitted = get_comm_size(comm) * count_bytes(recvtype, recvcount);
  COLLECTIVE_WRAPPER(OTF2_COLLECTIVE_OP_ALLTOALL,
                     transmitted,
                     transmitted);
  _LEAVE();
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_alltoallv(otf2_time_t start, otf2_time_t stop, int comm_size,
                           const int* sendcounts, mpi_type_t sendtype,
                           const int* recvcounts, mpi_type_t recvtype, comm_t comm)
{
  _ENTER("MPI_Alltoallv");
  int root = undefined_root;
  COLLECTIVE_WRAPPER(OTF2_COLLECTIVE_OP_ALLTOALLV,
                     count_bytes(sendtype, array_sum(sendcounts, comm_size)),
                     count_bytes(recvtype, array_sum(recvcounts, comm_size)));
  _LEAVE();
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_allreduce(otf2_time_t start, otf2_time_t stop, int count,
                           mpi_type_t type, comm_t comm)
{
  _ENTER("MPI_Allreduce");
  int root = undefined_root;
  int bytes = count_bytes(type, count) * get_comm_size(comm);
  COLLECTIVE_WRAPPER(OTF2_COLLECTIVE_OP_ALLREDUCE,
                     bytes,
                     bytes);
  _LEAVE();
}


OTF2_WRITER_RESULT
OTF2_Writer::mpi_reduce_scatter(otf2_time_t start, otf2_time_t stop, int comm_size,
                                const int* recvcounts, mpi_type_t type, comm_t comm)
{
  _ENTER("MPI_Reduce_scatter");
  int root = undefined_root;
  int sent = count_bytes(type, comm_size);
  int recv = comm_size * recvcounts[get_comm_rank(comm, world_.rank)] * type_sizes_[type];

  COLLECTIVE_WRAPPER(OTF2_COLLECTIVE_OP_REDUCE_SCATTER,
                     sent,
                     recv);
  _LEAVE();
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_group_union_first_pass(otf2_time_t start, otf2_time_t stop,
                             int group1, int group2, int newgroup)
{
  OTF2_MPI_Group& parent1 = groups_[group1];
  OTF2_MPI_Group& parent2 = groups_[group2];
  OTF2_MPI_Group& subGrp = groups_[newgroup];
  throw exception("Unimplemented: mpi_group_union");
  return OTF2_WRITER_SUCCESS;
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_group_union(otf2_time_t start, otf2_time_t stop,
                             int group1, int group2, int newgroup)
{
  _ENTER("MPI_Group_union");
  _LEAVE();
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_group_difference_first_pass(otf2_time_t start, otf2_time_t stop,
                                  int group1, int group2, int newgroup)
{
  OTF2_MPI_Group& parent1 = groups_[group1];
  OTF2_MPI_Group& parent2 = groups_[group2];
  OTF2_MPI_Group& subGrp = groups_[newgroup];
  throw exception("Unimplemented: mpi_group_difference");
  return OTF2_WRITER_SUCCESS;
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_group_difference(otf2_time_t start, otf2_time_t stop,
                                  int group1, int group2, int newgroup)
{
  _ENTER("MPI_Group_difference");
  _LEAVE();
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_group_intersection_first_pass(otf2_time_t start, otf2_time_t stop,
                                    int group1, int group2, int newgroup)
{
  OTF2_MPI_Group& parent1 = groups_[group1];
  OTF2_MPI_Group& parent2 = groups_[group2];
  OTF2_MPI_Group& subGrp = groups_[newgroup];
  throw exception("Unimplemented: mpi_group_intersection");
  return OTF2_WRITER_SUCCESS;
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_group_intersection(otf2_time_t start, otf2_time_t stop,
                                    int group1, int group2, int newgroup)
{
  _ENTER("MPI_Group_intersection");
  _LEAVE();
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_group_incl_first_pass(otf2_time_t start, otf2_time_t stop,
                                       int group, int count, const int* ranks, int newgroup)
{
  OTF2_MPI_Group& parent = groups_[group];
  OTF2_MPI_Group& subGrp = groups_[newgroup];

  subGrp.global_ranks.resize(count);
  for (int i=0; i < count; ++i){
    subGrp.global_ranks[i] = parent.global_ranks[ranks[i]];
  }
  return OTF2_WRITER_SUCCESS;
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_group_incl(otf2_time_t start, otf2_time_t stop,
                            int group, int count, const int* ranks, int newgroup)
{
  _ENTER("MPI_Group_incl");
  _LEAVE();
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_group_excl_first_pass(otf2_time_t start, otf2_time_t stop,
                                       int group, int count, const int* ranks, int newgroup)
{
  OTF2_MPI_Group& parent = groups_[group];
  OTF2_MPI_Group& subGrp = groups_[newgroup];

  std::set<int> sortedRanks;
  for (int i=0; i < count; ++i) sortedRanks.insert(ranks[i]);
  auto iter = sortedRanks.begin();

  int numParentRanks = parent.global_ranks.size();
  subGrp.global_ranks.resize(numParentRanks - count);
  int nextInsertIndex = 0;
  for (int i=0; i < numParentRanks; ++i){
    int nextToAvoid = *iter;
    int nextRank = parent.global_ranks[i];
    if (nextToAvoid == i){
      ++iter;
    } else {
      subGrp.global_ranks[nextInsertIndex] = nextRank;
      ++nextInsertIndex;
    }
  }
  return OTF2_WRITER_SUCCESS;
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_group_excl(otf2_time_t start, otf2_time_t stop,
                            int group, int count, const int* ranks, int newgroup)
{
  _ENTER("MPI_Group_excl");
  _LEAVE();
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_group_range_incl_first_pass(otf2_time_t start, otf2_time_t stop,
                                             int group, int count, int** ranges, int newgroup)
{
  OTF2_MPI_Group& parent = groups_[group];
  OTF2_MPI_Group& subGrp = groups_[newgroup];
  throw exception("Unimplemented: mpi_group_range_incl");
  return OTF2_WRITER_SUCCESS;
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_group_range_incl(otf2_time_t start, otf2_time_t stop,
                                  int group, int count, int**ranges, int newgroup)
{
  _ENTER("MPI_Group_range_incl");
  _LEAVE();
}

void
OTF2_Writer::agree_global_ids(const std::list<OTF2_MPI_Comm*>& subs,
                              global_id_assigner& assigner)
{
  assigner.add_level();
  for (OTF2_MPI_Comm* sub : subs){
    assigner.advance_sub_comm();
    agree_global_ids(sub, assigner);
  }
  assigner.remove_level();
}

void
OTF2_Writer::agree_global_ids(OTF2_MPI_Comm* comm, global_id_assigner& assigner)
{
  if (comm->is_root){
    assigner.assign_current();
  }

  if (comm->sub_comms.empty()) return;

  agree_global_ids(comm->sub_comms, assigner);
}

void
OTF2_Writer::agree_global_ids(global_id_assigner& assigner)
{
  OTF2_MPI_Comm& world = comms_[MPI_COMM_WORLD_ID];
  agree_global_ids(world.sub_comms, assigner);
}

void
OTF2_Writer::assign_global_ids(const std::list<OTF2_MPI_Comm*>& subs,
                              const global_id_assigner& global_ids,
                              tree_id& local_ids)
{
  local_ids.add_level();
  for (OTF2_MPI_Comm* sub : subs){
    local_ids.advance();
    assign_global_ids(sub, global_ids, local_ids);
  }
  local_ids.remove_level();
}

void
OTF2_Writer::assign_global_ids(OTF2_MPI_Comm* comm,
                              const global_id_assigner& global_ids,
                              tree_id& local_ids)
{
  int id = global_ids.get_id(local_ids);
  comm->global_id = id;

  if (comm->sub_comms.empty()) return;

  assign_global_ids(comm->sub_comms, global_ids, local_ids);
}

void
OTF2_Writer::assign_global_ids(const global_id_assigner& global_ids)
{
  tree_id local_ids;
  OTF2_MPI_Comm& world = comms_[MPI_COMM_WORLD_ID];
  assign_global_ids(world.sub_comms, global_ids, local_ids);
}

bool
OTF2_Writer::type_is_known(mpi_type_t type)
{
  if (type_sizes_.find(type) == type_sizes_.end()) {
    logger(OWV_WARN, std::string("Unknown Type (") + std::to_string(type));
    return false;
  }
  return true;
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_comm_dup_first_pass(otf2_time_t start, otf2_time_t stop,
                          comm_t comm, comm_t newcomm)
{
  OTF2_MPI_Comm& parent_comm = comms_[comm];
  OTF2_MPI_Comm& dup_comm = comms_[newcomm];
  dup_comm.local_id = newcomm;
  dup_comm.global_id = parent_comm.global_id;
  dup_comm.local_group_id = parent_comm.local_group_id;
  dup_comm.is_root = parent_comm.is_root;
  parent_comm.sub_comms.push_back(&dup_comm);
  return OTF2_WRITER_SUCCESS;
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_comm_dup(otf2_time_t start, otf2_time_t stop,
                          comm_t comm, comm_t newcomm)
{
  _ENTER("MPI_Comm_dup");
  _LEAVE();
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_comm_group(otf2_time_t start, otf2_time_t stop, comm_t comm, int group)
{
  _ENTER("MPI_Comm_group");
  _LEAVE();

  OTF2_MPI_Comm& comm_st = comms_[comm];
  if (comm_st.local_group_id != group){
    throw exception("mismatched commmunicator group in call to MPI_Comm_group");
  }
}


OTF2_WRITER_RESULT
OTF2_Writer::mpi_comm_create_first_pass(otf2_time_t start, otf2_time_t stop, comm_t comm,
                             int group, comm_t newcomm)
{
  //I don't know anything about groups yet - just build the tree
  OTF2_MPI_Comm& parent_comm = comms_[comm];
  OTF2_MPI_Comm& sub_comm = comms_[newcomm];
  OTF2_MPI_Group& subgrp = groups_[group];
  if (world_.rank == subgrp.global_ranks[0]){
    sub_comm.is_root = true;
  }
  parent_comm.sub_comms.push_back(&sub_comm);
  return OTF2_WRITER_SUCCESS;
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_comm_create(otf2_time_t start, otf2_time_t stop, comm_t comm,
                             int group, comm_t newcomm)
{
  _ENTER("MPI_Comm_create");
  _LEAVE();
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_comm_split_first_pass(otf2_time_t start, otf2_time_t stop, comm_t oldcomm,
                                       int key, int color, comm_t newcomm)
{
  /** TODO */
  return OTF2_WRITER_SUCCESS;
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_comm_split(otf2_time_t start, otf2_time_t stop, comm_t oldcomm,
                            int key, int color, comm_t newcomm)
{
  /** TODO
  OTF2_MPI_Comm& parent_comm = comms_[oldcomm];
  OTF2_MPI_Comm& child_comm = comms_[newcomm];
  group_t grpId = allocate_group_id();
  OTF2_MPI_Group& child_grp = groups_[grpId];
  child_grp.global_ranks = world_ranks;
  child_grp.local_id = grpId;
  child_comm.group = grpId;

  parent_comm.subComms[newcomm] = &child_comm;
  */

  _ENTER("MPI_Comm_split");
  _LEAVE();
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_type_contiguous(otf2_time_t start, otf2_time_t stop,
                                 int count, mpi_type_t oldtype, mpi_type_t newtype)
{
  _ENTER("MPI_Type_contiguous");

  if (!type_is_known(oldtype)){
    logger(OWV_ERROR, std::string("MPI_type_contiguous failed"));
    return OTF2_WRITER_ERROR_UKNOWN_MPI_TYPE;
  }

  type_sizes_[newtype] = count_bytes(oldtype, count);

  _LEAVE();
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_type_hvector(otf2_time_t start, otf2_time_t stop, int count,
                              int blocklength, mpi_type_t oldtype, mpi_type_t newtype)
{
  _ENTER("MPI_Type_hvector");
  mpi_t_vector_inner("MPI_Type_hvector", count, blocklength, oldtype, newtype);
  _LEAVE();
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_type_vector(otf2_time_t start, otf2_time_t stop, int count,
                             int blocklength, mpi_type_t oldtype, mpi_type_t newtype)
{
  _ENTER("MPI_Type_vector");
  mpi_t_vector_inner("MPI_Type_vector", count, blocklength, oldtype, newtype);
  _LEAVE();
}

void
OTF2_Writer::mpi_t_vector_inner(const char* fname, int count, int blocklength,
                                mpi_type_t oldtype, mpi_type_t newtype)
{
  if (!type_is_known(oldtype)){
    logger(OWV_ERROR, std::string(fname) + " failed");
    return;
  }
  type_sizes_[newtype] = count_bytes(oldtype, blocklength) * count;
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_type_indexed(otf2_time_t start, otf2_time_t stop, int count,
                              const int* lengths, mpi_type_t oldtype, mpi_type_t newtype)
{
  _ENTER("MPI_Type_indexed");
  mpi_t_indexed_inner("MPI_Type_indexed", count, lengths, oldtype, newtype);
  _LEAVE();
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_type_hindexed(otf2_time_t start, otf2_time_t stop, int count,
                               const int* lengths, mpi_type_t oldtype, mpi_type_t newtype)
{
  _ENTER("MPI_Type_hindexed");
  mpi_t_indexed_inner("MPI_Type_hindexed", count, lengths, oldtype, newtype);
  _LEAVE();
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_type_create_hindexed(otf2_time_t start, otf2_time_t stop, int count,
                                      const int*lengths, mpi_type_t oldtype, mpi_type_t newtype)
{
  _ENTER("MPI_Type_hindexed");
  mpi_t_indexed_inner("MPI_Type_hindexed", count, lengths, oldtype, newtype);
  _LEAVE();
}

void
OTF2_Writer::mpi_t_indexed_inner(const char* name, int count, const int* lengths,
                                 mpi_type_t oldtype, mpi_type_t newtype)
{
  if (!type_is_known(oldtype)){
    logger(OWV_ERROR, std::string(name) + " failed");
    return;
  }

  type_sizes_[newtype] = count_bytes(oldtype, array_sum(lengths, count));
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_type_struct(otf2_time_t start, otf2_time_t stop, int count,
                             const int* blocklengths, mpi_type_t* oldtypes, mpi_type_t newtype)
{
  _ENTER("MPI_Type_struct");
  mpi_t_struct_inner("MPI_Type_struct", count, blocklengths, oldtypes, newtype);
  _LEAVE();
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_type_create_struct(otf2_time_t start, otf2_time_t stop, int count,
                                    const int* blocklengths, mpi_type_t* oldtypes, mpi_type_t newtype)
{
  _ENTER("MPI_Type_create_struct");
  mpi_t_struct_inner("MPI_Type_create_struct", count, blocklengths, oldtypes, newtype);
  _LEAVE();
}

void
OTF2_Writer::mpi_t_struct_inner(const char* fname, int count, const int* blocklengths,
                                mpi_type_t* oldtypes, mpi_type_t newtype)
{
  int sum = 0;
  for(int i = 0; i < count; i++) {
    if (!type_is_known(oldtypes[i])){
      logger(OWV_ERROR, std::string(fname) + "failed");
      return;
    }
    sum += count_bytes(oldtypes[i], blocklengths[i]);
  }

  type_sizes_[newtype] = sum;
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_type_create_subarray(otf2_time_t start, otf2_time_t stop,
                                      int ndims, const int* subsizes, mpi_type_t oldtype, mpi_type_t newtype)
{
  _ENTER("MPI_Type_create_subarray");

  if (!type_is_known(oldtype)){
    logger(OWV_ERROR, std::string("MPI_Type_create_subarray failed"));
    return OTF2_WRITER_ERROR_UKNOWN_MPI_TYPE;
  }

  type_sizes_[newtype] = count_bytes(oldtype, array_sum(subsizes, ndims));

  _LEAVE();
}

OTF2_WRITER_RESULT
OTF2_Writer::mpi_type_create_hvector(otf2_time_t start, otf2_time_t stop, int count,
                                     int blocklength, mpi_type_t oldtype, mpi_type_t newtype)
{
  _ENTER("MPI_Type_create_hvector");
  mpi_t_vector_inner("MPI_Type_create_hvector", count, blocklength, oldtype, newtype);
  _LEAVE();
}

int
OTF2_Writer::get_world_rank(int comm_rank, int comm)
{
  //TODO
  return 0;
}

int
OTF2_Writer::get_comm_rank(int world_rank, comm_t comm)
{
  //TODO
  return 0;
}

int
OTF2_Writer::get_comm_size(comm_t comm)
{
  //TODO
  return 0;
}

uint64_t
OTF2_Writer::count_bytes(mpi_type_t type, uint64_t count)
{
 if (type_sizes_.find(type) == type_sizes_.end()) {
   logger(OWV_ERROR, std::string("Unkown data type (")
          + std::to_string((int)type) + "). Assuming 4 bytes in size.");
   return 4*count;
 } else {
   return type_sizes_[type]*count;
 }
}


}