#include "msap1/acquisition_ipc.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace msap1 {

struct AcquisitionClient::Impl {
	explicit Impl(std::string socket_path)
		: work(boost::asio::make_work_guard(context)),
		  client(std::make_shared<mnc::ipc::RequestClient>(
			  context.get_executor(), std::move(socket_path))),
		  thread([this] { context.run(); })
	{
		/* Connect lazily from request().  Besides allowing a persistent client
		 * to be constructed before the service is ready, this keeps transport
		 * failures inside the product-level AcquisitionUnavailable boundary. */
	}

	~Impl()
	{
		client->close();
		work.reset();
		context.stop();
		if (thread.joinable())
			thread.join();
	}

	boost::asio::io_context context;
	boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work;
	std::shared_ptr<mnc::ipc::RequestClient> client;
	std::mutex connect_mutex;
	std::thread thread;
};

AcquisitionClient::AcquisitionClient(std::string socket_path)
	: impl_(std::make_unique<Impl>(std::move(socket_path)))
{
}

AcquisitionClient::~AcquisitionClient() = default;
AcquisitionClient::AcquisitionClient(AcquisitionClient &&) noexcept = default;
AcquisitionClient &AcquisitionClient::operator=(AcquisitionClient &&) noexcept =
	default;

mnc::ipc::Frame AcquisitionClient::transport(mnc::ipc::Frame frame,
					     int timeout_ms)
{
	/* Correlation IDs belong to the transport.  RequestClient assigns them,
	 * which prevents the collisions a caller-generated timestamp produced
	 * when parallel HTTP handlers submitted requests in the same tick. */
	try {
		if (!impl_->client->is_open()) {
			/* Only one caller establishes the persistent stream. Requests remain
			 * concurrent after connection because RequestClient serializes its
			 * own frame queue and correlation map on an Asio strand. */
			std::scoped_lock connect_lock(impl_->connect_mutex);
			if (!impl_->client->is_open())
				boost::asio::co_spawn(impl_->context,
					impl_->client->connect(), boost::asio::use_future)
					.get();
		}
		return boost::asio::co_spawn(
			impl_->context,
			impl_->client->request(
				std::move(frame),
				std::chrono::milliseconds(timeout_ms)),
			boost::asio::use_future)
			.get();
	} catch (const std::exception &error) {
		/* An EOF or reset can leave the native socket looking open until the
		 * reader coroutine observes it. Explicit invalidation makes the next
		 * product request establish a fresh stream. Side-effecting requests are
		 * not retried here because the lost response may follow a successful
		 * operation on the server. */
		impl_->client->close();
		throw AcquisitionUnavailable(error.what());
	}
}

} // namespace msap1
