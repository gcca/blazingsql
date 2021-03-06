#include <unistd.h>
#include <clocale>

#include <arpa/inet.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <execinfo.h>
#include <signal.h>

#include <ucp/api/ucp.h>

#include <spdlog/spdlog.h>
#include <spdlog/async.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include <algorithm>
#include <cuda_runtime.h>
#include <memory>
#include <chrono>
#include <thread>         // std::this_thread::sleep_for
#include <fstream>
#include <utility>
#include <memory>

#include <blazingdb/io/Config/BlazingContext.h>
#include <blazingdb/io/Library/Logging/CoutOutput.h>
#include <blazingdb/io/Library/Logging/Logger.h>
#include "blazingdb/io/Library/Logging/ServiceLogging.h"
#include <blazingdb/io/Util/StringUtil.h>

#include "communication/ucx_init.h"
#include "communication/CommunicationData.h"
#include "communication/CommunicationInterface/protocols.hpp"

#include <bmr/initializer.h>
#include <bmr/BlazingMemoryResource.h>

#include "utilities/error.hpp"

#include "cudf/detail/gather.hpp"
#include "communication/CommunicationInterface/node.hpp"
#include "communication/CommunicationInterface/protocols.hpp"
#include "communication/CommunicationInterface/messageSender.hpp"
#include "communication/CommunicationInterface/messageListener.hpp"
#include "execution_kernels/kernel.h"
#include "execution_graph/executor.h"

using namespace fmt::literals;

#include "cache_machine/CacheMachine.h"

#include "engine/initialize.h"
#include "engine/static.h" // this contains function call for getProductDetails
#include "engine/engine.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>


using namespace fmt::literals;
using namespace ral::cache;

/**
	@brief This class has a counter of the number of blazingcontext we have in the process
*/
class blazing_context_ref_counter {
public:
    static blazing_context_ref_counter& getInstance() {
        // Myers' singleton. Thread safe and unique. Note: C++11 required.
        static blazing_context_ref_counter instance;
        return instance;
    }

    size_t get_count() const { return this->count; }
    void increase() { ++count; } // +1
    void decrease() { assert(this->count > 0); --count; } // -1 (assert if the count < 0)

private:
    size_t count = 0;
};

void handler(int sig) {
  void *array[10];
  size_t size;
  // get void*'s for all entries on the stack
  size = backtrace(array, 10);
  // print out all the frames to stderr
  fprintf(stderr, "Error: signal %d:\n", sig);
  backtrace_symbols_fd(array, size, STDERR_FILENO);
  exit(1);
}

std::string get_ip(const std::string & iface_name = "eno1") {
	int fd;
	struct ifreq ifr;

	fd = socket(AF_INET, SOCK_DGRAM, 0);

	/* I want to get an IPv4 IP address */
	ifr.ifr_addr.sa_family = AF_INET;

	/* I want IP address attached to "eth0" */
	strncpy(ifr.ifr_name, iface_name.c_str(), IFNAMSIZ - 1);

	ioctl(fd, SIOCGIFADDR, &ifr);

	close(fd);

	/* display result */
	// printf("%s\n", inet_ntoa(((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr));
	const std::string the_ip(inet_ntoa(((struct sockaddr_in *) &ifr.ifr_addr)->sin_addr));

	return the_ip;
}

auto log_level_str_to_enum(std::string level) {
	if (level == "critical") {
		return spdlog::level::critical;
	}
	else if (level == "err") {
		return spdlog::level::err;
	}
	else if (level == "info") {
		return spdlog::level::info;
	}
	else if (level == "debug") {
		return spdlog::level::debug;
	}
	else if (level == "trace") {
		return spdlog::level::trace;
	}
	else if (level == "warn") {
		return spdlog::level::warn;
	}
	else {
		return spdlog::level::off;
	}
}

// simple_log: true (no timestamp or log level)
void create_logger(std::string fileName,
	std::string loggingName,
	uint16_t ralId, std::string flush_level,
	std::string logger_level_wanted,
	std::size_t max_size_logging,
	bool simple_log=true) {

	std::shared_ptr<spdlog::logger> existing_logger = spdlog::get(loggingName);
	if (existing_logger){ // if logger already exists, dont initialize again
		return;
	}

	auto stdout_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
	stdout_sink->set_pattern("[%T.%e] [%^%l%$] %v");
	stdout_sink->set_level(spdlog::level::err);

	// TODO: discuss how we should handle this
	// if max_num_files = 4 -> will have: RAL.0.log, RAL.0.1.log, RAL.0.2.log, RAL.0.3.log, RAL.0.4.log
	auto max_num_files = 0;
	auto rotating_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt> (fileName, max_size_logging, max_num_files);
	if (simple_log) {
		rotating_sink->set_pattern(fmt::format("%v"));
	} else {
		rotating_sink->set_pattern(fmt::format("%Y-%m-%d %T.%e|{}|%^%l%$|%v", ralId));
	}

	// We want ALL levels of info to be registered. So using by default `trace` level
	rotating_sink->set_level(spdlog::level::trace);
	spdlog::sinks_init_list sink_list = {stdout_sink, rotating_sink};
	auto logger = std::make_shared<spdlog::async_logger>(loggingName, sink_list, spdlog::thread_pool(), spdlog::async_overflow_policy::block);

	// level of logs
	logger->set_level(log_level_str_to_enum(logger_level_wanted));

	spdlog::register_logger(logger);

	spdlog::flush_on(log_level_str_to_enum(flush_level));
	spdlog::flush_every(std::chrono::seconds(1));
}


//=============================================================================

bool isEmptyFile(std::ifstream& pFile)
{
    return pFile.peek() == std::ifstream::traits_type::eof();
}

void printLoggerHeader(const std::string pathLogger, const std::string nameLogger){
    const std::map<std::string, std::string> headers = {
        {"queries_logger",      "ral_id|query_id|start_time|plan|query"},
        {"kernels_logger",      "ral_id|query_id|kernel_id|is_kernel|kernel_type|description"},
        {"kernels_edges_logger","ral_id|query_id|source|sink"},
        {"task_logger",         "time_started|ral_id|query_id|kernel_id|duration_decaching|duration_execution|input_num_rows|input_num_bytes"},
        {"cache_events_logger", "ral_id|query_id|message_id|cache_id|num_rows|num_bytes|event_type|timestamp_begin|timestamp_end|description"},
        {"batch_logger",        "log_time|node_id|type|query_id|step|substep|info|duration|extra1|data1|extra2|data2"},
        {"input_comms",         "unique_id|ral_id|query_id|kernel_id|dest_ral_id|dest_ral_count|dest_cache_id|message_id|phase"},
        {"output_comms",        "unique_id|ral_id|query_id|kernel_id|dest_ral_id|dest_ral_count|dest_cache_id|message_id|phase"}
    };

    std::ifstream fileLogger(pathLogger);
    if(fileLogger.good()) {
        bool emptyLogger = isEmptyFile(fileLogger);

        if(emptyLogger){
            std::shared_ptr<spdlog::logger> logger = spdlog::get(nameLogger);
            if(logger){
                logger->info(headers.at(nameLogger));
            }
        }
    }
}


std::mutex initialize_lock;
bool initialized = false;

/**
* Initializes the engine and gives us shared pointers to both our transport out cache
* and the cache we use for receiving messages
*
*/
std::pair<std::pair<std::shared_ptr<CacheMachine>,std::shared_ptr<CacheMachine> >, int> initialize(uint16_t ralId,
	std::string worker_id,
	std::string network_iface_name,
	int ralCommunicationPort,
	std::vector<NodeMetaDataUCP> workers_ucp_info,
	bool singleNode,
	std::map<std::string, std::string> config_options,
	std::string allocation_mode,
	std::size_t initial_pool_size,
	std::size_t maximum_pool_size,
	bool enable_logging) {

	std::lock_guard<std::mutex> init_lock(initialize_lock);

	float device_mem_resouce_consumption_thresh = 0.6;
	auto config_it = config_options.find("BLAZING_DEVICE_MEM_CONSUMPTION_THRESHOLD");
	if (config_it != config_options.end()){
		device_mem_resouce_consumption_thresh = std::stof(config_options["BLAZING_DEVICE_MEM_CONSUMPTION_THRESHOLD"]);
	}
	std::string logging_dir = "blazing_log";
	config_it = config_options.find("BLAZING_LOGGING_DIRECTORY");
	if (config_it != config_options.end()){
		logging_dir = config_options["BLAZING_LOGGING_DIRECTORY"];
	}
	bool logging_directory_missing = false;
	struct stat sb;
	if (!(stat(logging_dir.c_str(), &sb) == 0 && S_ISDIR(sb.st_mode))){ // logging_dir does not exist
	// we are assuming that this logging directory was created by the python layer, because only the python layer can only target on directory creation per server
	// having all RALs independently trying to create a directory simulatenously can cause problems
		logging_directory_missing = true;
		logging_dir = "";
	}

	std::string allocator_logging_file = "";
	if (enable_logging && !logging_directory_missing){
		allocator_logging_file = logging_dir + "/allocator." + std::to_string(ralId) + ".log";
	}
	BlazingRMMInitialize(allocation_mode, initial_pool_size, maximum_pool_size, allocator_logging_file, device_mem_resouce_consumption_thresh);

	// ---------------------------------------------------------------------------
	// DISCLAIMER
	// TODO: Support proper locale support for non-US cases (percy)
	std::setlocale(LC_ALL, "en_US.UTF-8");
	std::setlocale(LC_NUMERIC, "en_US.UTF-8");
	// ---------------------------------------------------------------------------

	//signal(SIGSEGV, handler);   // install our handler. This is for debugging.

	std::string ralHost = get_ip(network_iface_name);

	std::string initLogMsg = "INITIALIZING RAL. RAL ID: " + std::to_string(ralId)  + ", ";
	initLogMsg = initLogMsg + "RAL Host: " + ralHost + ":" + std::to_string(ralCommunicationPort) + ", ";
	initLogMsg = initLogMsg + "Network Interface: " + network_iface_name + ", ";
	initLogMsg = initLogMsg + (singleNode ? ", Is Single Node, " : ", Is Not Single Node, ");

	const char * env_cuda_device = std::getenv("CUDA_VISIBLE_DEVICES");
	std::string env_cuda_device_str = env_cuda_device == nullptr ? "" : std::string(env_cuda_device);
	initLogMsg = initLogMsg + "CUDA_VISIBLE_DEVICES is set to: " + env_cuda_device_str + ", ";


	bool require_acknowledge = false;
	// TODO: the require acknowledge feature is currently not working, for now it is permanently disabled
	// auto iter = config_options.find("REQUIRE_ACKNOWLEDGE");
	// if (iter != config_options.end()){
	// 	require_acknowledge = (config_options["REQUIRE_ACKNOWLEDGE"] == "true" ||
	// 							config_options["REQUIRE_ACKNOWLEDGE"] == "True" ||
	// 							config_options["REQUIRE_ACKNOWLEDGE"] == "1" ||
	// 							config_options["REQUIRE_ACKNOWLEDGE"] == "TRUE" );
	// }

	size_t buffers_size = 1048576;  // 1 MBs
	auto iter = config_options.find("TRANSPORT_BUFFER_BYTE_SIZE");
	if (iter != config_options.end()){
		buffers_size = std::stoi(config_options["TRANSPORT_BUFFER_BYTE_SIZE"]);
	}
	int num_comm_threads = 20;
	iter = config_options.find("MAX_SEND_MESSAGE_THREADS");
	if (iter != config_options.end()){
		num_comm_threads = std::stoi(config_options["MAX_SEND_MESSAGE_THREADS"]);
	}
	int num_buffers = 1000;
	iter = config_options.find("TRANSPORT_POOL_NUM_BUFFERS");
	if (iter != config_options.end()){
		num_buffers = std::stoi(config_options["TRANSPORT_POOL_NUM_BUFFERS"]);
	}

	//to avoid redundancy the default value or user defined value for this parameter is placed on the pyblazing side
	assert( config_options.find("BLAZ_HOST_MEM_CONSUMPTION_THRESHOLD") != config_options.end() );
	float host_memory_quota = std::stof(config_options["BLAZ_HOST_MEM_CONSUMPTION_THRESHOLD"]);

	blazing_host_memory_resource::getInstance().initialize(host_memory_quota);

	// Init AWS S3
	BlazingContext::getInstance()->initExternalSystems();

	int executor_threads = 10;
	auto exec_it = config_options.find("EXECUTOR_THREADS");
	if (exec_it != config_options.end()){
		executor_threads = std::stoi(config_options["EXECUTOR_THREADS"]);
	}
	
	std::string flush_level = "warn";
	
	auto log_it = config_options.find("LOGGING_FLUSH_LEVEL");
	if (log_it != config_options.end()){
		flush_level = config_options["LOGGING_FLUSH_LEVEL"];
	}

	std::string enable_general_engine_logs;
    log_it = config_options.find("ENABLE_GENERAL_ENGINE_LOGS");
    if (log_it != config_options.end()){
        enable_general_engine_logs = config_options["ENABLE_GENERAL_ENGINE_LOGS"];
    }

    std::string enable_comms_logs;
    log_it = config_options.find("ENABLE_COMMS_LOGS");
    if (log_it != config_options.end()){
        enable_comms_logs = config_options["ENABLE_COMMS_LOGS"];
    }

    std::string enable_task_logs;
    log_it = config_options.find("ENABLE_TASK_LOGS");
    if (log_it != config_options.end()){
        enable_task_logs = config_options["ENABLE_TASK_LOGS"];
    }

    std::string enable_other_engine_logs;
    log_it = config_options.find("ENABLE_OTHER_ENGINE_LOGS");
    if (log_it != config_options.end()){
        enable_other_engine_logs = config_options["ENABLE_OTHER_ENGINE_LOGS"];
    }

	std::string logger_level_wanted = "trace";
	auto log_level_it = config_options.find("LOGGING_LEVEL");
	if (log_level_it != config_options.end()){
		logger_level_wanted = config_options["LOGGING_LEVEL"];
	}

	std::size_t max_size_logging = 1073741824; // 1 GB
	auto size_log_it = config_options.find("LOGGING_MAX_SIZE_PER_FILE");
	if (size_log_it != config_options.end()){
		max_size_logging = std::stoi(config_options["LOGGING_MAX_SIZE_PER_FILE"]);
	}

	comm::blazing_protocol protocol = comm::blazing_protocol::tcp;
	std::string protocol_value = StringUtil::toLower(config_options["PROTOCOL"]);
	if (protocol_value == "ucx"){
		protocol = comm::blazing_protocol::ucx;
	}

	if (!initialized){

		// spdlog batch logger
		spdlog::shutdown();

		spdlog::init_thread_pool(8192, 1);

		if(enable_general_engine_logs=="True") {
		    std::string batchLoggerFileName = logging_dir + "/RAL." + std::to_string(ralId) + ".log";
		    create_logger(batchLoggerFileName, "batch_logger", ralId, flush_level, logger_level_wanted, max_size_logging, false);
		    printLoggerHeader(batchLoggerFileName, "batch_logger");
        }

        if(enable_comms_logs=="True"){
            std::string outputCommunicationLoggerFileName = logging_dir + "/output_comms." + std::to_string(ralId) + ".log";
            create_logger(outputCommunicationLoggerFileName, "output_comms", ralId, flush_level, logger_level_wanted, max_size_logging);
            printLoggerHeader(outputCommunicationLoggerFileName, "output_comms");

            std::string inputCommunicationLoggerFileName = logging_dir + "/input_comms." + std::to_string(ralId) + ".log";
            create_logger(inputCommunicationLoggerFileName, "input_comms", ralId, flush_level, logger_level_wanted, max_size_logging);
            printLoggerHeader(inputCommunicationLoggerFileName, "input_comms");
        }

        if(enable_other_engine_logs=="True"){
            std::string queriesFileName = logging_dir + "/bsql_queries." + std::to_string(ralId) + ".log";
            create_logger(queriesFileName, "queries_logger", ralId, flush_level, logger_level_wanted, max_size_logging);
            printLoggerHeader(queriesFileName, "queries_logger");

            std::string kernelsFileName = logging_dir + "/bsql_kernels." + std::to_string(ralId) + ".log";
            create_logger(kernelsFileName, "kernels_logger", ralId, flush_level, logger_level_wanted, max_size_logging);
            printLoggerHeader(kernelsFileName, "kernels_logger");

            std::string kernelsEdgesFileName = logging_dir + "/bsql_kernels_edges." + std::to_string(ralId) + ".log";
            create_logger(kernelsEdgesFileName, "kernels_edges_logger", ralId, flush_level, logger_level_wanted, max_size_logging);
            printLoggerHeader(kernelsEdgesFileName, "kernels_edges_logger");

			std::string cacheEventsFileName = logging_dir + "/bsql_cache_events." + std::to_string(ralId) + ".log";
            create_logger(cacheEventsFileName, "cache_events_logger", ralId, flush_level, logger_level_wanted, max_size_logging);
            printLoggerHeader(cacheEventsFileName, "cache_events_logger");

        }

        if(enable_task_logs=="True"){
            std::string tasksFileName = logging_dir + "/bsql_kernel_tasks." + std::to_string(ralId) + ".log";
            create_logger(tasksFileName, "task_logger", ralId, flush_level, logger_level_wanted, max_size_logging);
            printLoggerHeader(tasksFileName, "task_logger");
        }
	} 

	std::shared_ptr<spdlog::logger> logger = spdlog::get("batch_logger");
	if (logging_directory_missing){
	    if(logger){
		    logger->error("|||{info}|||||","info"_a="BLAZING_LOGGING_DIRECTORY not found. It was not created.");
	    }
	}

	if(logger){
	    logger->debug("|||{info}|||||","info"_a=initLogMsg);
	}
	std::map<std::string, std::string> product_details = getProductDetails();
	std::string product_details_str = "Product Details: ";
	std::map<std::string, std::string>::iterator it = product_details.begin();
	while (it != product_details.end())	{
		product_details_str += it->first + ": " + it->second + "; ";
		it++;
	}
	if(logger){
	    logger->debug("|||{info}|||||","info"_a=product_details_str);
	}


	blazing_device_memory_resource* resource = &blazing_device_memory_resource::getInstance();
	std::string alloc_info = "allocation_mode: " + allocation_mode;
	alloc_info += ", total_memory: " + std::to_string(resource->get_total_memory());
	alloc_info += ", memory_limit: " + std::to_string(resource->get_memory_limit());
	alloc_info += ", type: " + resource->get_type();
	alloc_info += ", initial_pool_size: " + std::to_string(initial_pool_size);
	alloc_info += ", maximum_pool_size: " + std::to_string(maximum_pool_size);
	alloc_info += ", allocator_logging_file: " + allocator_logging_file;
	if(logger){
	    logger->debug("|||{info}|||||","info"_a=alloc_info);
	}


	std::string orc_files_path;
	iter = config_options.find("BLAZING_CACHE_DIRECTORY");
	if (iter != config_options.end()) {
		orc_files_path = config_options["BLAZING_CACHE_DIRECTORY"];
	}
	if (!singleNode) {
		orc_files_path += std::to_string(ralId);
	}

	auto & communicationData = ral::communication::CommunicationData::getInstance();
	communicationData.initialize(worker_id, orc_files_path);

	auto output_input_caches = std::make_pair(std::make_shared<CacheMachine>(nullptr, "messages_out", false,CACHE_LEVEL_CPU ),std::make_shared<CacheMachine>(nullptr, "messages_in", false));

	ucp_context_h ucp_context = nullptr;
	// start ucp servers
	if(!singleNode){
		std::map<std::string, comm::node> nodes_info_map;

		ucp_worker_h self_worker = nullptr;
		if(protocol == comm::blazing_protocol::ucx){
			ucp_context = reinterpret_cast<ucp_context_h>(workers_ucp_info[0].context_handle);

			self_worker = ral::communication::CreatetUcpWorker(ucp_context);

			ral::communication::UcpWorkerAddress ucpWorkerAddress = ral::communication::GetUcpWorkerAddress(self_worker);

			std::map<std::string, ral::communication::UcpWorkerAddress> peer_addresses_map;
			auto th = std::thread([ralCommunicationPort, total_peers=workers_ucp_info.size(), &peer_addresses_map, worker_id, workers_ucp_info](){
				ral::communication::AddressExchangerForSender exchanger(ralCommunicationPort);
				for (size_t i = 0; i < total_peers; i++){
					if(workers_ucp_info[i].worker_id == worker_id){
						continue;
					}
					if (exchanger.acceptConnection()){
						int ret;

						// Receive worker_id size
						size_t worker_id_buff_size;
						ret = recv(exchanger.fd(), &worker_id_buff_size, sizeof(size_t), MSG_WAITALL);
						ral::communication::CheckError(static_cast<size_t>(ret) != sizeof(size_t), "recv worker_id_buff_size");

						// Receive worker_id
						std::string worker_id(worker_id_buff_size, '\0');
						ret = recv(exchanger.fd(), &worker_id[0], worker_id.size(), MSG_WAITALL);
						ral::communication::CheckError(static_cast<size_t>(ret) != worker_id.size(), "recv worker_id");

						// Receive ucp_worker_address size
						size_t ucp_worker_address_size;
						ret = recv(exchanger.fd(), &ucp_worker_address_size, sizeof(size_t), MSG_WAITALL);
						ral::communication::CheckError(static_cast<size_t>(ret) != sizeof(size_t), "recv ucp_worker_address_size");

						// Receive ucp_worker_address
						std::uint8_t *data = new std::uint8_t[ucp_worker_address_size];
						ral::communication::UcpWorkerAddress peerUcpWorkerAddress{
								reinterpret_cast<ucp_address_t *>(data),
								ucp_worker_address_size};

						ret = recv(exchanger.fd(), peerUcpWorkerAddress.address, ucp_worker_address_size, MSG_WAITALL);
						ral::communication::CheckError(static_cast<size_t>(ret) != ucp_worker_address_size, "recv ucp_worker_address");

						peer_addresses_map.emplace(worker_id, peerUcpWorkerAddress);

						exchanger.closeCurrentConnection();
					}
				}
			});

			std::this_thread::sleep_for(std::chrono::seconds(1));
			for (auto &&worker_info : workers_ucp_info){
				if(worker_info.worker_id == worker_id){
					continue;
				}
				ral::communication::AddressExchangerForReceiver exchanger(worker_info.port, worker_info.ip.c_str());
				int ret;

				// Send worker_id size
				size_t worker_id_buff_size = worker_id.size();
				ret = send(exchanger.fd(), &worker_id_buff_size, sizeof(size_t), 0);
				ral::communication::CheckError(static_cast<size_t>(ret) != sizeof(size_t), "send worker_id_buff_size");

				// Send worker_id
				ret = send(exchanger.fd(), worker_id.data(), worker_id.size(), 0);
				ral::communication::CheckError(static_cast<size_t>(ret) != worker_id.size(), "send worker_id");

				// Send ucp_worker_address size
				ret = send(exchanger.fd(), &ucpWorkerAddress.length, sizeof(size_t), 0);
				ral::communication::CheckError(static_cast<size_t>(ret) != sizeof(size_t), "send ucp_worker_address_size");

				// Send ucp_worker_address
				ret = send(exchanger.fd(), ucpWorkerAddress.address, ucpWorkerAddress.length, 0);
				ral::communication::CheckError(static_cast<size_t>(ret) != ucpWorkerAddress.length, "send ucp_worker_address");
			}

			th.join();

			for (auto &&worker_info : workers_ucp_info){
				if(worker_info.worker_id == worker_id){
					continue;
				}
				ral::communication::UcpWorkerAddress peerUcpWorkerAddress = peer_addresses_map[worker_info.worker_id];
				ucp_ep_h ucp_ep = ral::communication::CreateUcpEp(self_worker, peerUcpWorkerAddress);

				// std::cout << '[' << std::hex << std::this_thread::get_id()
				// 					<< "] local: " << std::hex
				// 					<< *reinterpret_cast<std::size_t *>(ucpWorkerAddress.address) << ' '
				// 					<< ucpWorkerAddress.length << std::endl
				// 					<< '[' << std::hex << std::this_thread::get_id()
				// 					<< "] peer: " << std::hex
				// 					<< *reinterpret_cast<std::size_t *>(peerUcpWorkerAddress.address)
				// 					<<' ' << peerUcpWorkerAddress.length << std::endl;

				// auto worker_info = workers_ucp_info[0];
				nodes_info_map.emplace(worker_info.worker_id, comm::node(ralId, worker_info.worker_id, ucp_ep, self_worker));
			}

			comm::ucx_message_listener::initialize_message_listener(
				ucp_context, self_worker,nodes_info_map,20, output_input_caches.second);
			comm::ucx_message_listener::get_instance()->poll_begin_message_tag(true);
			output_input_caches.second = comm::ucx_message_listener::get_instance()->get_input_cache();

		}else{


			for (auto &&worker_info : workers_ucp_info){
				if(worker_info.worker_id == worker_id){
					continue;
				}

				nodes_info_map.emplace(worker_info.worker_id, comm::node(ralId, worker_info.worker_id, worker_info.ip, worker_info.port));
			}

			comm::tcp_message_listener::initialize_message_listener(nodes_info_map,ralCommunicationPort,num_comm_threads, output_input_caches.second);
			comm::tcp_message_listener::get_instance()->start_polling();
			ralCommunicationPort = comm::tcp_message_listener::get_instance()->get_port(); // if the listener was already initialized, we want to get the port that was originally set and send that back to python side
			output_input_caches.second = comm::tcp_message_listener::get_instance()->get_input_cache();
		}
		comm::message_sender::initialize_instance(output_input_caches.first,
			nodes_info_map,
			num_comm_threads, ucp_context, self_worker, ralId,protocol,require_acknowledge);
		comm::message_sender::get_instance()->run_polling();

		output_input_caches.first = comm::message_sender::get_instance()->get_output_cache();
	}

	bool map_ucx = protocol == comm::blazing_protocol::ucx;
	ral::memory::set_allocation_pools(buffers_size, num_buffers,
										buffers_size, num_buffers, map_ucx, ucp_context);

	double processing_memory_limit_threshold = 0.9;
	config_it = config_options.find("BLAZING_PROCESSING_DEVICE_MEM_CONSUMPTION_THRESHOLD");
	if (config_it != config_options.end()){
		processing_memory_limit_threshold = std::stod(config_options["BLAZING_PROCESSING_DEVICE_MEM_CONSUMPTION_THRESHOLD"]);
	}

	ral::execution::executor::init_executor(executor_threads, processing_memory_limit_threshold);
	initialized = true;
  blazing_context_ref_counter::getInstance().increase();
	return std::make_pair(output_input_caches, ralCommunicationPort);	
}

void clear_graphs(std::vector<int32_t> ctx_tokens) {
  for (auto& ctx_token : ctx_tokens) {
    //printf("will deregister the graph with token: %d\n", ctx_token);
    auto graph = comm::graphs_info::getInstance().get_graph(ctx_token);
    if (graph != nullptr) {
      //printf("\tnot null\n");
      try {
        // TODO percy felipe william rommel threads/futures in c++ cannot be cancel by 
        // itself we need to program that logic: We should implement some sort 
        // of interruption logic for the kernel threads/futures and then clean 
        // its memory. For now we are using getExecuteGraphResult for the 
        // finalize caller and this will ensure to free all the memory.
        // Related with -> https://github.com/BlazingDB/blazingsql/issues/1363
        auto freeThis = getExecuteGraphResult(graph, ctx_token);
        // printf("\tdone!\n");
      } catch(const std::exception& e) {
        throw;
      }
    }
   }
}

void finalize(std::vector<int32_t> ctx_tokens) {
  clear_graphs(ctx_tokens);

  if (blazing_context_ref_counter::getInstance().get_count() == 1) {
    BlazingContext::getInstance()->shutDownExternalSystems();

    // TODO BlazingRMMFinalize and cudaDeviceReset percy william felipe rommel
    // if we want to finalize all the engine properly we need to implement 
    // first many parts: thread interruptions, free memory from incomplete 
    // threads, notify every ral node to shutdown the processing, etc.
    // More details here -> https://github.com/BlazingDB/blazingsql/issues/1363

    // BlazingRMMFinalize();

    spdlog::shutdown();

    //cudaDeviceReset();
  }

  blazing_context_ref_counter::getInstance().decrease();
}

error_code_t initialize_C(uint16_t ralId,
	std::string worker_id,
	std::string network_iface_name,

	int ralCommunicationPort,
	std::vector<NodeMetaDataUCP> workers_ucp_info,
	bool singleNode,
	std::map<std::string, std::string> config_options,
	std::string allocation_mode,
	std::size_t initial_pool_size,
	std::size_t maximum_pool_size,
	bool enable_logging) {

	try {
		initialize(ralId,
			worker_id,
			network_iface_name,
			ralCommunicationPort,
			workers_ucp_info,
			singleNode,
			config_options,
			allocation_mode,
			initial_pool_size,
			maximum_pool_size,
			enable_logging);
		return E_SUCCESS;
	} catch (std::exception& e) {
		return E_EXCEPTION;
	}
}

error_code_t finalize_C(std::vector<int32_t> ctx_tokens) {
	try {
		finalize(ctx_tokens);
		return E_SUCCESS;
	} catch (std::exception& e) {
		return E_EXCEPTION;
	}
}

size_t getFreeMemory() {
	BlazingMemoryResource* resource = &blazing_device_memory_resource::getInstance();
	size_t total_free_memory = resource->get_memory_limit() - resource->get_memory_used();
	return total_free_memory;
}

void resetMaxMemoryUsed(int to) {
	blazing_device_memory_resource* resource = &blazing_device_memory_resource::getInstance();
	resource->reset_max_memory_used(to);
}

size_t getMaxMemoryUsed() {
    blazing_device_memory_resource* resource = &blazing_device_memory_resource::getInstance();
	return resource->get_max_memory_used();
}
