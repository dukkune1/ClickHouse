#include <Common/ZooKeeper/TestKeeperStorage.h>
#include <Common/ZooKeeper/IKeeper.h>
#include <Common/setThreadName.h>
#include <mutex>
#include <functional>
#include <common/logger_useful.h>
#include <Common/StringUtils/StringUtils.h>
#include <sstream>
#include <iomanip>

namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
    extern const int BAD_ARGUMENTS;
}

}

namespace zkutil
{

using namespace DB;

static String parentPath(const String & path)
{
    auto rslash_pos = path.rfind('/');
    if (rslash_pos > 0)
        return path.substr(0, rslash_pos);
    return "/";
}

static String baseName(const String & path)
{
    auto rslash_pos = path.rfind('/');
    return path.substr(rslash_pos + 1);
}

static TestKeeperStorage::ResponsesForSessions processWatchesImpl(const String & path, TestKeeperStorage::Watches & watches, TestKeeperStorage::Watches & list_watches, Coordination::Event event_type)
{
    TestKeeperStorage::ResponsesForSessions result;
    auto it = watches.find(path);
    if (it != watches.end())
    {
        std::shared_ptr<Coordination::ZooKeeperWatchResponse> watch_response = std::make_shared<Coordination::ZooKeeperWatchResponse>();
        watch_response->path = path;
        watch_response->xid = -1;
        watch_response->zxid = -1;
        watch_response->type = event_type;
        watch_response->state = Coordination::State::CONNECTED;
        for (auto watcher_session : it->second)
            result.push_back(TestKeeperStorage::ResponseForSession{watcher_session, watch_response});

        watches.erase(it);
    }

    auto parent_path = parentPath(path);
    it = list_watches.find(parent_path);
    if (it != list_watches.end())
    {
        std::shared_ptr<Coordination::ZooKeeperWatchResponse> watch_list_response = std::make_shared<Coordination::ZooKeeperWatchResponse>();
        watch_list_response->path = parent_path;
        watch_list_response->xid = -1;
        watch_list_response->zxid = -1;
        watch_list_response->type = Coordination::Event::CHILD;
        watch_list_response->state = Coordination::State::CONNECTED;
        for (auto watcher_session : it->second)
            result.push_back(TestKeeperStorage::ResponseForSession{watcher_session, watch_list_response});

        list_watches.erase(it);
    }
    return result;
}

TestKeeperStorage::TestKeeperStorage()
{
    container.emplace("/", Node());
}

using Undo = std::function<void()>;

struct TestKeeperStorageRequest
{
    Coordination::ZooKeeperRequestPtr zk_request;

    explicit TestKeeperStorageRequest(const Coordination::ZooKeeperRequestPtr & zk_request_)
        : zk_request(zk_request_)
    {}
    virtual std::pair<Coordination::ZooKeeperResponsePtr, Undo> process(TestKeeperStorage::Container & container, TestKeeperStorage::Ephemerals & ephemerals, int64_t zxid, int64_t session_id) const = 0;
    virtual TestKeeperStorage::ResponsesForSessions processWatches(TestKeeperStorage::Watches & /*watches*/, TestKeeperStorage::Watches & /*list_watches*/) const { return {}; }

    virtual ~TestKeeperStorageRequest() = default;
};

struct TestKeeperStorageHeartbeatRequest final : public TestKeeperStorageRequest
{
    using TestKeeperStorageRequest::TestKeeperStorageRequest;
    std::pair<Coordination::ZooKeeperResponsePtr, Undo> process(TestKeeperStorage::Container & /* container */, TestKeeperStorage::Ephemerals & /* ephemerals */, int64_t /* zxid */, int64_t /* session_id */) const override
    {
        return {zk_request->makeResponse(), {}};
    }
};


struct TestKeeperStorageCreateRequest final : public TestKeeperStorageRequest
{
    using TestKeeperStorageRequest::TestKeeperStorageRequest;

    TestKeeperStorage::ResponsesForSessions processWatches(TestKeeperStorage::Watches & watches, TestKeeperStorage::Watches & list_watches) const override
    {
        return processWatchesImpl(zk_request->getPath(), watches, list_watches, Coordination::Event::CREATED);
    }

    std::pair<Coordination::ZooKeeperResponsePtr, Undo> process(TestKeeperStorage::Container & container, TestKeeperStorage::Ephemerals & ephemerals, int64_t zxid, int64_t session_id) const override
    {
        Coordination::ZooKeeperResponsePtr response_ptr = zk_request->makeResponse();
        Undo undo;
        Coordination::ZooKeeperCreateResponse & response = dynamic_cast<Coordination::ZooKeeperCreateResponse &>(*response_ptr);
        Coordination::ZooKeeperCreateRequest & request = dynamic_cast<Coordination::ZooKeeperCreateRequest &>(*zk_request);

        if (container.count(request.path))
        {
            response.error = Coordination::Error::ZNODEEXISTS;
        }
        else
        {
            auto it = container.find(parentPath(request.path));

            if (it == container.end())
            {
                response.error = Coordination::Error::ZNONODE;
            }
            else if (it->second.is_ephemeral)
            {
                response.error = Coordination::Error::ZNOCHILDRENFOREPHEMERALS;
            }
            else
            {
                TestKeeperStorage::Node created_node;
                created_node.seq_num = 0;
                created_node.stat.czxid = zxid;
                created_node.stat.mzxid = zxid;
                created_node.stat.ctime = std::chrono::system_clock::now().time_since_epoch() / std::chrono::milliseconds(1);
                created_node.stat.mtime = created_node.stat.ctime;
                created_node.stat.numChildren = 0;
                created_node.stat.dataLength = request.data.length();
                created_node.data = request.data;
                created_node.is_ephemeral = request.is_ephemeral;
                created_node.is_sequental = request.is_sequential;
                std::string path_created = request.path;

                if (request.is_sequential)
                {
                    auto seq_num = it->second.seq_num;

                    std::stringstream seq_num_str;      // STYLE_CHECK_ALLOW_STD_STRING_STREAM
                    seq_num_str.exceptions(std::ios::failbit);
                    seq_num_str << std::setw(10) << std::setfill('0') << seq_num;

                    path_created += seq_num_str.str();
                }

                /// Increment sequential number even if node is not sequential
                ++it->second.seq_num;

                response.path_created = path_created;
                container.emplace(path_created, std::move(created_node));

                if (request.is_ephemeral)
                    ephemerals[session_id].emplace(path_created);

                undo = [&container, &ephemerals, session_id, path_created, is_ephemeral = request.is_ephemeral, parent_path = it->first]
                {
                    container.erase(path_created);
                    if (is_ephemeral)
                        ephemerals[session_id].erase(path_created);
                    auto & undo_parent = container.at(parent_path);
                    --undo_parent.stat.cversion;
                    --undo_parent.stat.numChildren;
                    --undo_parent.seq_num;
                };

                ++it->second.stat.cversion;
                ++it->second.stat.numChildren;

                response.error = Coordination::Error::ZOK;
            }
        }

        return { response_ptr, undo };
    }
};

struct TestKeeperStorageGetRequest final : public TestKeeperStorageRequest
{
    using TestKeeperStorageRequest::TestKeeperStorageRequest;
    std::pair<Coordination::ZooKeeperResponsePtr, Undo> process(TestKeeperStorage::Container & container, TestKeeperStorage::Ephemerals & /* ephemerals */, int64_t /* zxid */, int64_t /* session_id */) const override
    {
        Coordination::ZooKeeperResponsePtr response_ptr = zk_request->makeResponse();
        Coordination::ZooKeeperGetResponse & response = dynamic_cast<Coordination::ZooKeeperGetResponse &>(*response_ptr);
        Coordination::ZooKeeperGetRequest & request = dynamic_cast<Coordination::ZooKeeperGetRequest &>(*zk_request);

        auto it = container.find(request.path);
        if (it == container.end())
        {
            response.error = Coordination::Error::ZNONODE;
        }
        else
        {
            response.stat = it->second.stat;
            response.data = it->second.data;
            response.error = Coordination::Error::ZOK;
        }

        return { response_ptr, {} };
    }
};

struct TestKeeperStorageRemoveRequest final : public TestKeeperStorageRequest
{
    using TestKeeperStorageRequest::TestKeeperStorageRequest;
    std::pair<Coordination::ZooKeeperResponsePtr, Undo> process(TestKeeperStorage::Container & container, TestKeeperStorage::Ephemerals & ephemerals, int64_t /*zxid*/, int64_t session_id) const override
    {
        Coordination::ZooKeeperResponsePtr response_ptr = zk_request->makeResponse();
        Coordination::ZooKeeperRemoveResponse & response = dynamic_cast<Coordination::ZooKeeperRemoveResponse &>(*response_ptr);
        Coordination::ZooKeeperRemoveRequest & request = dynamic_cast<Coordination::ZooKeeperRemoveRequest &>(*zk_request);
        Undo undo;

        auto it = container.find(request.path);
        if (it == container.end())
        {
            response.error = Coordination::Error::ZNONODE;
        }
        else if (request.version != -1 && request.version != it->second.stat.version)
        {
            response.error = Coordination::Error::ZBADVERSION;
        }
        else if (it->second.stat.numChildren)
        {
            response.error = Coordination::Error::ZNOTEMPTY;
        }
        else
        {
            auto prev_node = it->second;
            if (prev_node.is_ephemeral)
                ephemerals[session_id].erase(request.path);

            container.erase(it);
            auto & parent = container.at(parentPath(request.path));
            --parent.stat.numChildren;
            ++parent.stat.cversion;
            response.error = Coordination::Error::ZOK;

            undo = [prev_node, &container, &ephemerals, session_id, path = request.path]
            {
                if (prev_node.is_ephemeral)
                    ephemerals[session_id].emplace(path);

                container.emplace(path, prev_node);
                auto & undo_parent = container.at(parentPath(path));
                ++undo_parent.stat.numChildren;
                --undo_parent.stat.cversion;
            };
        }

        return { response_ptr, undo };
    }

    TestKeeperStorage::ResponsesForSessions processWatches(TestKeeperStorage::Watches & watches, TestKeeperStorage::Watches & list_watches) const override
    {
        return processWatchesImpl(zk_request->getPath(), watches, list_watches, Coordination::Event::DELETED);
    }
};

struct TestKeeperStorageExistsRequest final : public TestKeeperStorageRequest
{
    using TestKeeperStorageRequest::TestKeeperStorageRequest;
    std::pair<Coordination::ZooKeeperResponsePtr, Undo> process(TestKeeperStorage::Container & container, TestKeeperStorage::Ephemerals & /* ephemerals */, int64_t /*zxid*/, int64_t /* session_id */) const override
    {
        Coordination::ZooKeeperResponsePtr response_ptr = zk_request->makeResponse();
        Coordination::ZooKeeperExistsResponse & response = dynamic_cast<Coordination::ZooKeeperExistsResponse &>(*response_ptr);
        Coordination::ZooKeeperExistsRequest & request = dynamic_cast<Coordination::ZooKeeperExistsRequest &>(*zk_request);

        auto it = container.find(request.path);
        if (it != container.end())
        {
            response.stat = it->second.stat;
            response.error = Coordination::Error::ZOK;
        }
        else
        {
            response.error = Coordination::Error::ZNONODE;
        }

        return { response_ptr, {} };
    }
};

struct TestKeeperStorageSetRequest final : public TestKeeperStorageRequest
{
    using TestKeeperStorageRequest::TestKeeperStorageRequest;
    std::pair<Coordination::ZooKeeperResponsePtr, Undo> process(TestKeeperStorage::Container & container, TestKeeperStorage::Ephemerals & /* ephemerals */, int64_t zxid, int64_t /* session_id */) const override
    {
        Coordination::ZooKeeperResponsePtr response_ptr = zk_request->makeResponse();
        Coordination::ZooKeeperSetResponse & response = dynamic_cast<Coordination::ZooKeeperSetResponse &>(*response_ptr);
        Coordination::ZooKeeperSetRequest & request = dynamic_cast<Coordination::ZooKeeperSetRequest &>(*zk_request);
        Undo undo;

        auto it = container.find(request.path);
        if (it == container.end())
        {
            response.error = Coordination::Error::ZNONODE;
        }
        else if (request.version == -1 || request.version == it->second.stat.version)
        {
            auto prev_node = it->second;

            it->second.data = request.data;
            ++it->second.stat.version;
            it->second.stat.mzxid = zxid;
            it->second.stat.mtime = std::chrono::system_clock::now().time_since_epoch() / std::chrono::milliseconds(1);
            it->second.stat.dataLength = request.data.length();
            it->second.data = request.data;
            ++container.at(parentPath(request.path)).stat.cversion;
            response.stat = it->second.stat;
            response.error = Coordination::Error::ZOK;

            undo = [prev_node, &container, path = request.path]
            {
                container.at(path) = prev_node;
                --container.at(parentPath(path)).stat.cversion;
            };
        }
        else
        {
            response.error = Coordination::Error::ZBADVERSION;
        }

        return { response_ptr, undo };
    }

    TestKeeperStorage::ResponsesForSessions processWatches(TestKeeperStorage::Watches & watches, TestKeeperStorage::Watches & list_watches) const override
    {
        return processWatchesImpl(zk_request->getPath(), watches, list_watches, Coordination::Event::CHANGED);
    }

};

struct TestKeeperStorageListRequest final : public TestKeeperStorageRequest
{
    using TestKeeperStorageRequest::TestKeeperStorageRequest;
    std::pair<Coordination::ZooKeeperResponsePtr, Undo> process(TestKeeperStorage::Container & container, TestKeeperStorage::Ephemerals & /* ephemerals */, int64_t /*zxid*/, int64_t /*session_id*/) const override
    {
        Coordination::ZooKeeperResponsePtr response_ptr = zk_request->makeResponse();
        Coordination::ZooKeeperListResponse & response = dynamic_cast<Coordination::ZooKeeperListResponse &>(*response_ptr);
        Coordination::ZooKeeperListRequest & request = dynamic_cast<Coordination::ZooKeeperListRequest &>(*zk_request);
        auto it = container.find(request.path);
        if (it == container.end())
        {
            response.error = Coordination::Error::ZNONODE;
        }
        else
        {
            auto path_prefix = request.path;
            if (path_prefix.empty())
                throw DB::Exception("Logical error: path cannot be empty", ErrorCodes::LOGICAL_ERROR);

            if (path_prefix.back() != '/')
                path_prefix += '/';

            /// Fairly inefficient.
            for (auto child_it = container.upper_bound(path_prefix);
                 child_it != container.end() && startsWith(child_it->first, path_prefix);
                ++child_it)
            {
                if (parentPath(child_it->first) == request.path)
                    response.names.emplace_back(baseName(child_it->first));
            }

            response.stat = it->second.stat;
            response.error = Coordination::Error::ZOK;
        }

        return { response_ptr, {} };
    }
};

struct TestKeeperStorageCheckRequest final : public TestKeeperStorageRequest
{
    using TestKeeperStorageRequest::TestKeeperStorageRequest;
    std::pair<Coordination::ZooKeeperResponsePtr, Undo> process(TestKeeperStorage::Container & container, TestKeeperStorage::Ephemerals & /* ephemerals */, int64_t /*zxid*/, int64_t /*session_id*/) const override
    {
        Coordination::ZooKeeperResponsePtr response_ptr = zk_request->makeResponse();
        Coordination::ZooKeeperCheckResponse & response = dynamic_cast<Coordination::ZooKeeperCheckResponse &>(*response_ptr);
        Coordination::ZooKeeperCheckRequest & request = dynamic_cast<Coordination::ZooKeeperCheckRequest &>(*zk_request);
        auto it = container.find(request.path);
        if (it == container.end())
        {
            response.error = Coordination::Error::ZNONODE;
        }
        else if (request.version != -1 && request.version != it->second.stat.version)
        {
            response.error = Coordination::Error::ZBADVERSION;
        }
        else
        {
            response.error = Coordination::Error::ZOK;
        }

        return { response_ptr, {} };
    }
};

struct TestKeeperStorageMultiRequest final : public TestKeeperStorageRequest
{
    std::vector<TestKeeperStorageRequestPtr> concrete_requests;
    explicit TestKeeperStorageMultiRequest(const Coordination::ZooKeeperRequestPtr & zk_request_)
        : TestKeeperStorageRequest(zk_request_)
    {
        Coordination::ZooKeeperMultiRequest & request = dynamic_cast<Coordination::ZooKeeperMultiRequest &>(*zk_request);
        concrete_requests.reserve(request.requests.size());

        for (const auto & sub_request : request.requests)
        {
            auto sub_zk_request = std::dynamic_pointer_cast<Coordination::ZooKeeperRequest>(sub_request);
            if (sub_zk_request->getOpNum() == Coordination::OpNum::Create)
            {
                concrete_requests.push_back(std::make_shared<TestKeeperStorageCreateRequest>(sub_zk_request));
            }
            else if (sub_zk_request->getOpNum() == Coordination::OpNum::Remove)
            {
                concrete_requests.push_back(std::make_shared<TestKeeperStorageRemoveRequest>(sub_zk_request));
            }
            else if (sub_zk_request->getOpNum() == Coordination::OpNum::Set)
            {
                concrete_requests.push_back(std::make_shared<TestKeeperStorageSetRequest>(sub_zk_request));
            }
            else if (sub_zk_request->getOpNum() == Coordination::OpNum::Check)
            {
                concrete_requests.push_back(std::make_shared<TestKeeperStorageCheckRequest>(sub_zk_request));
            }
            else
                throw DB::Exception(ErrorCodes::BAD_ARGUMENTS, "Illegal command as part of multi ZooKeeper request {}", sub_zk_request->getOpNum());
        }
    }

    std::pair<Coordination::ZooKeeperResponsePtr, Undo> process(TestKeeperStorage::Container & container, TestKeeperStorage::Ephemerals & ephemerals, int64_t zxid, int64_t session_id) const override
    {
        Coordination::ZooKeeperResponsePtr response_ptr = zk_request->makeResponse();
        Coordination::ZooKeeperMultiResponse & response = dynamic_cast<Coordination::ZooKeeperMultiResponse &>(*response_ptr);
        std::vector<Undo> undo_actions;

        try
        {
            size_t i = 0;
            for (const auto & concrete_request : concrete_requests)
            {
                auto [ cur_response, undo_action ] = concrete_request->process(container, ephemerals, zxid, session_id);

                response.responses[i] = cur_response;
                if (cur_response->error != Coordination::Error::ZOK)
                {
                    for (size_t j = 0; j <= i; ++j)
                    {
                        auto response_error = response.responses[j]->error;
                        response.responses[j] = std::make_shared<Coordination::ZooKeeperErrorResponse>();
                        response.responses[j]->error = response_error;
                    }

                    for (size_t j = i + 1; j < response.responses.size(); ++j)
                    {
                        response.responses[j] = std::make_shared<Coordination::ZooKeeperErrorResponse>();
                        response.responses[j]->error = Coordination::Error::ZRUNTIMEINCONSISTENCY;
                    }

                    for (auto it = undo_actions.rbegin(); it != undo_actions.rend(); ++it)
                        if (*it)
                            (*it)();

                    return { response_ptr, {} };
                }
                else
                    undo_actions.emplace_back(std::move(undo_action));

                ++i;
            }

            response.error = Coordination::Error::ZOK;
            return { response_ptr, {} };
        }
        catch (...)
        {
            for (auto it = undo_actions.rbegin(); it != undo_actions.rend(); ++it)
                if (*it)
                    (*it)();
            throw;
        }
    }

    TestKeeperStorage::ResponsesForSessions processWatches(TestKeeperStorage::Watches & watches, TestKeeperStorage::Watches & list_watches) const override
    {
        TestKeeperStorage::ResponsesForSessions result;
        for (const auto & generic_request : concrete_requests)
        {
            auto responses = generic_request->processWatches(watches, list_watches);
            result.insert(result.end(), responses.begin(), responses.end());
        }
        return result;
    }
};

struct TestKeeperStorageCloseRequest final : public TestKeeperStorageRequest
{
    using TestKeeperStorageRequest::TestKeeperStorageRequest;
    std::pair<Coordination::ZooKeeperResponsePtr, Undo> process(TestKeeperStorage::Container &, TestKeeperStorage::Ephemerals &, int64_t, int64_t) const override
    {
        throw DB::Exception("Called process on close request", ErrorCodes::LOGICAL_ERROR);
    }
};

TestKeeperStorage::ResponsesForSessions TestKeeperStorage::finalize(const RequestsForSessions & expired_requests)
{
    if (finalized)
        throw DB::Exception("Testkeeper storage already finalized", ErrorCodes::LOGICAL_ERROR);

    finalized = true;

    ResponsesForSessions finalize_results;
    auto finish_watch = [] (const auto & watch_pair) -> ResponsesForSessions
    {
        ResponsesForSessions results;
        std::shared_ptr<Coordination::ZooKeeperWatchResponse> response = std::make_shared<Coordination::ZooKeeperWatchResponse>();
        response->type = Coordination::SESSION;
        response->state = Coordination::EXPIRED_SESSION;
        response->error = Coordination::Error::ZSESSIONEXPIRED;

        for (auto & watcher_session : watch_pair.second)
            results.push_back(ResponseForSession{watcher_session, response});
        return results;
    };

    for (auto & path_watch : watches)
    {
        auto watch_responses = finish_watch(path_watch);
        finalize_results.insert(finalize_results.end(), watch_responses.begin(), watch_responses.end());
    }

    watches.clear();
    for (auto & path_watch : list_watches)
    {
        auto list_watch_responses = finish_watch(path_watch);
        finalize_results.insert(finalize_results.end(), list_watch_responses.begin(), list_watch_responses.end());
    }
    list_watches.clear();
    sessions_and_watchers.clear();

    for (const auto & [session_id, zk_request] : expired_requests)
    {
        auto response = zk_request->makeResponse();
        response->error = Coordination::Error::ZSESSIONEXPIRED;
        finalize_results.push_back(ResponseForSession{session_id, response});
    }
    return finalize_results;
}


class TestKeeperWrapperFactory final : private boost::noncopyable
{

public:
    using Creator = std::function<TestKeeperStorageRequestPtr(const Coordination::ZooKeeperRequestPtr &)>;
    using OpNumToRequest = std::unordered_map<Coordination::OpNum, Creator>;

    static TestKeeperWrapperFactory & instance()
    {
        static TestKeeperWrapperFactory factory;
        return factory;
    }

    TestKeeperStorageRequestPtr get(const Coordination::ZooKeeperRequestPtr & zk_request) const
    {
        auto it = op_num_to_request.find(zk_request->getOpNum());
        if (it == op_num_to_request.end())
            throw DB::Exception("Unknown operation type " + toString(zk_request->getOpNum()), ErrorCodes::LOGICAL_ERROR);

        return it->second(zk_request);
    }

    void registerRequest(Coordination::OpNum op_num, Creator creator)
    {
        if (!op_num_to_request.try_emplace(op_num, creator).second)
            throw DB::Exception(ErrorCodes::LOGICAL_ERROR, "Request with op num {} already registered", op_num);
    }

private:
    OpNumToRequest op_num_to_request;
    TestKeeperWrapperFactory();
};

template<Coordination::OpNum num, typename RequestT>
void registerTestKeeperRequestWrapper(TestKeeperWrapperFactory & factory)
{
    factory.registerRequest(num, [] (const Coordination::ZooKeeperRequestPtr & zk_request) { return std::make_shared<RequestT>(zk_request); });
}


TestKeeperWrapperFactory::TestKeeperWrapperFactory()
{
    registerTestKeeperRequestWrapper<Coordination::OpNum::Heartbeat, TestKeeperStorageHeartbeatRequest>(*this);
    //registerTestKeeperRequestWrapper<Coordination::OpNum::Auth, TestKeeperStorageAuthRequest>(*this);
    registerTestKeeperRequestWrapper<Coordination::OpNum::Close, TestKeeperStorageCloseRequest>(*this);
    registerTestKeeperRequestWrapper<Coordination::OpNum::Create, TestKeeperStorageCreateRequest>(*this);
    registerTestKeeperRequestWrapper<Coordination::OpNum::Remove, TestKeeperStorageRemoveRequest>(*this);
    registerTestKeeperRequestWrapper<Coordination::OpNum::Exists, TestKeeperStorageExistsRequest>(*this);
    registerTestKeeperRequestWrapper<Coordination::OpNum::Get, TestKeeperStorageGetRequest>(*this);
    registerTestKeeperRequestWrapper<Coordination::OpNum::Set, TestKeeperStorageSetRequest>(*this);
    registerTestKeeperRequestWrapper<Coordination::OpNum::List, TestKeeperStorageListRequest>(*this);
    registerTestKeeperRequestWrapper<Coordination::OpNum::SimpleList, TestKeeperStorageListRequest>(*this);
    registerTestKeeperRequestWrapper<Coordination::OpNum::Check, TestKeeperStorageCheckRequest>(*this);
    registerTestKeeperRequestWrapper<Coordination::OpNum::Multi, TestKeeperStorageMultiRequest>(*this);
}


TestKeeperStorage::ResponsesForSessions TestKeeperStorage::processRequest(const Coordination::ZooKeeperRequestPtr & zk_request, int64_t session_id)
{
    TestKeeperStorage::ResponsesForSessions results;
    if (zk_request->getOpNum() == Coordination::OpNum::Close)
    {
        auto it = ephemerals.find(session_id);
        if (it != ephemerals.end())
        {
            for (const auto & ephemeral_path : it->second)
            {
                container.erase(ephemeral_path);
                auto responses = processWatchesImpl(ephemeral_path, watches, list_watches, Coordination::Event::DELETED);
                results.insert(results.end(), responses.begin(), responses.end());
            }
            ephemerals.erase(it);
        }
        clearDeadWatches(session_id);

        /// Finish connection
        auto response = std::make_shared<Coordination::ZooKeeperCloseResponse>();
        response->xid = zk_request->xid;
        response->zxid = getZXID();
        results.push_back(ResponseForSession{session_id, response});
    }
    else
    {

        TestKeeperStorageRequestPtr storage_request = TestKeeperWrapperFactory::instance().get(zk_request);
        auto [response, _] = storage_request->process(container, ephemerals, zxid, session_id);

        if (zk_request->has_watch)
        {
            if (response->error == Coordination::Error::ZOK)
            {
                auto & watches_type = zk_request->getOpNum() == Coordination::OpNum::List || zk_request->getOpNum() == Coordination::OpNum::SimpleList
                    ? list_watches
                    : watches;

                watches_type[zk_request->getPath()].emplace_back(session_id);
                sessions_and_watchers[session_id].emplace(zk_request->getPath());
            }
            else if (response->error == Coordination::Error::ZNONODE && zk_request->getOpNum() == Coordination::OpNum::Exists)
            {
                watches[zk_request->getPath()].emplace_back(session_id);
                sessions_and_watchers[session_id].emplace(zk_request->getPath());
            }
            else
            {
                std::shared_ptr<Coordination::ZooKeeperWatchResponse> watch_response = std::make_shared<Coordination::ZooKeeperWatchResponse>();
                watch_response->path = zk_request->getPath();
                watch_response->xid = -1;
                watch_response->error = response->error;
                watch_response->type = Coordination::Event::NOTWATCHING;
                results.push_back(ResponseForSession{session_id, watch_response});
            }
        }

        if (response->error == Coordination::Error::ZOK)
        {
            auto watch_responses = storage_request->processWatches(watches, list_watches);
            results.insert(results.end(), watch_responses.begin(), watch_responses.end());
        }

        response->xid = zk_request->xid;
        response->zxid = getZXID();

        results.push_back(ResponseForSession{session_id, response});
    }

    return results;
}


void TestKeeperStorage::clearDeadWatches(int64_t session_id)
{
    auto watches_it = sessions_and_watchers.find(session_id);
    if (watches_it != sessions_and_watchers.end())
    {
        for (const auto & watch_path : watches_it->second)
        {
            auto watch = watches.find(watch_path);
            if (watch != watches.end())
            {
                auto & watches_for_path = watch->second;
                for (auto w_it = watches_for_path.begin(); w_it != watches_for_path.end();)
                {
                    if (*w_it == session_id)
                        w_it = watches_for_path.erase(w_it);
                    else
                        ++w_it;
                }
                if (watches_for_path.empty())
                    watches.erase(watch);
            }

            auto list_watch = list_watches.find(watch_path);
            if (list_watch != list_watches.end())
            {
                auto & list_watches_for_path = list_watch->second;
                for (auto w_it = list_watches_for_path.begin(); w_it != list_watches_for_path.end();)
                {
                    if (*w_it == session_id)
                        w_it = list_watches_for_path.erase(w_it);
                    else
                        ++w_it;
                }
                if (list_watches_for_path.empty())
                    list_watches.erase(list_watch);
            }
        }
        sessions_and_watchers.erase(watches_it);
    }
}

}
