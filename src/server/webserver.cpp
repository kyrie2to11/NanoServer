#include "webserver.h"

using namespace std;

WebServer::WebServer(
            int port, int trigMode, int timeoutMS, bool OptLinger,
            int sqlPort, const char* sqlUser, const  char* sqlPwd,
            const char* dbName, int connPoolNum, int threadNum,
            bool openLog, int logLevel, int logQueSize):
            port_(port), openLinger_(OptLinger), timeoutMS_(timeoutMS), isClose_(false),
            timer_(new HeapTimer()), threadpool_(new ThreadPool(threadNum)), epoller_(new Epoller()) // timer_ threadpool_ epoller_ 初始化
    {
    srcDir_ = getcwd(nullptr, 256); // 当前工作路径：启动 server 时，终端中显示的当前路径
    assert(srcDir_);
    strncat(srcDir_, "/resources/", 16);
    HttpConn::userCount = 0;
    HttpConn::srcDir = srcDir_;
    SqlConnPool::Instance()->Init("localhost", sqlPort, sqlUser, sqlPwd, dbName, connPoolNum); // sql 连接池初始化

    InitEventMode_(trigMode); // 事件模式初始化
    if(!InitSocket_()) { isClose_ = true;} // 监听 socket 初始化（创建 listenFd_ 并加入到 epoller_ 监听事件集合中）

    if(openLog) {
        Log::Instance()->init(logLevel, "./log", ".log", logQueSize); // log 初始化
        if(isClose_) { LOG_ERROR("========== Server init error!=========="); }
        else {
            LOG_INFO("========== Server init ==========");
            LOG_INFO("Port:%d, OpenLinger: %s", port_, OptLinger? "true":"false");
            LOG_INFO("Listen Mode: %s, OpenConn Mode: %s",
                            (listenEvent_ & EPOLLET ? "ET": "LT"),
                            (connEvent_ & EPOLLET ? "ET": "LT"));
            LOG_INFO("LogSys level: %d", logLevel);
            LOG_INFO("srcDir: %s", HttpConn::srcDir);
            LOG_INFO("SqlConnPool num: %d, ThreadPool num: %d", connPoolNum, threadNum);
        }
    }
}

WebServer::~WebServer() {
    close(listenFd_);
    isClose_ = true;
    free(srcDir_);
}

void WebServer::InitEventMode_(int trigMode) {
    // EPOLLRDHUP： 对端连接半关闭状态。在 TCP 连接中，当对端发送了 FIN 包，表示对端已经没有数据要发送了，也就是对端关闭了写通道，但读通道可能还保持打开状态，这种情况就会触发 EPOLLRDHUP 事件。
    // EPOLLONESHOT：当一个文件描述符（如套接字）被设置为EPOLLONESHOT模式并且触发了一个事件后，在这个事件被处理完成之前，epoll不会再为这个文件描述符触发相同类型的事件。
    // 这样做的好处是可以保证在同一时刻只有一个线程在处理该文件描述符对应的事件，避免多个线程同时处理一个连接可能导致的并发问题，比如数据混乱、重复处理等情况。
    listenEvent_ = EPOLLRDHUP; 
    connEvent_ = EPOLLONESHOT | EPOLLRDHUP;
    switch (trigMode)
    {
    case 0:
        break;
    case 1:
        connEvent_ |= EPOLLET; // 连接套接字启用边缘触发模式
        break;
    case 2:
        listenEvent_ |= EPOLLET; // 监听套接字启用边缘触发模式
        break;
    case 3:
        // 监听套接字和连接套接字同时启用边缘触发模式
        listenEvent_ |= EPOLLET;
        connEvent_ |= EPOLLET;
        break;
    default:
        listenEvent_ |= EPOLLET;
        connEvent_ |= EPOLLET;
        break;
    }
    HttpConn::isET = (connEvent_ & EPOLLET); // 位与操作结果非 0 则启用了边缘触发模式
}

void WebServer::Start() {
    /* Reactor: DealListen() 没有调用线程池中的线程，DealRead_() 和 DealWrite_() 则交由线程池中的线程处理 */
    int timeMS = -1;  /* epoll wait timeout == -1 无事件将阻塞 */
    if(!isClose_) { LOG_INFO("========== Server start =========="); }
    while(!isClose_) {
        if(timeoutMS_ > 0) {
            timeMS = timer_->GetNextTick();
        }
        int eventCnt = epoller_->Wait(timeMS);
        for(int i = 0; i < eventCnt; i++) {
            /* 处理事件 */
            int fd = epoller_->GetEventFd(i);
            uint32_t event = epoller_->GetEvents(i);
            if(fd == listenFd_) { // 收到 http 连接请求,建立新的 socket 与之沟通
                DealListen_();
            }
            else if(event & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                assert(users_.count(fd) > 0);
                CloseConn_(&users_[fd]);
            }
            else if(event & EPOLLIN) { // http 连接发来读请求
                assert(users_.count(fd) > 0);
                DealRead_(&users_[fd]);
            }
            else if(event & EPOLLOUT) { // http 连接发来写请求
                assert(users_.count(fd) > 0);
                DealWrite_(&users_[fd]);
            } else {
                LOG_ERROR("Unexpected event");
            }
        }
    }
}

void WebServer::SendError_(int fd, const char*info) {
    assert(fd > 0);
    int ret = send(fd, info, strlen(info), 0);
    if(ret < 0) {
        LOG_WARN("send error to client[%d] error!", fd);
    }
    close(fd);
}

void WebServer::CloseConn_(HttpConn* client) {
    assert(client);
    LOG_INFO("Client[%d] quit!", client->GetFd());
    epoller_->DelFd(client->GetFd());
    client->Close();
}

void WebServer::AddClient_(int fd, sockaddr_in addr) {
    assert(fd > 0);
    users_[fd].init(fd, addr); // HttpConn 初始化 （内部包含 request_ 的初始化）
    if(timeoutMS_ > 0) {
        timer_->add(fd, timeoutMS_, std::bind(&WebServer::CloseConn_, this, &users_[fd])); // std::bind() 返回一个新的可调用对象
    }
    epoller_->AddFd(fd, EPOLLIN | connEvent_);
    SetFdNonblock(fd);
    LOG_INFO("Client[%d] in!", users_[fd].GetFd());
}

void WebServer::DealListen_() {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    do {
        // 等待监听 socket(listenFd_) 来 http 连接，打开一个新的 socket(fd) 与之交流, addr 存储对端的 ip:port
        // 如果没有新的 http 连接到来, 返回的 fd = -1
        int fd = accept(listenFd_, (struct sockaddr *)&addr, &len); 
        if(fd <= 0) { 
            LOG_INFO("No new http connection arrived! current http connection userCount: %d", (int)HttpConn::userCount);
            return;
        }
        else if(HttpConn::userCount >= MAX_FD) {
            SendError_(fd, "Server busy!");
            LOG_WARN("Clients is full!");
            return;
        }
        AddClient_(fd, addr); // 参数解释： 服务端（webserver）处理浏览器 http 连接的 socket fd, 客户端（浏览器）对应的 socket address ip:port
    } while(listenEvent_ & EPOLLET); // 当监听事件处于边缘触发模式时
}

void WebServer::DealRead_(HttpConn* client) {
    assert(client);
    ExtentTime_(client);
    threadpool_->AddTask(std::bind(&WebServer::OnRead_, this, client)); // std::bind() 返回一个新的可调用对象
}

void WebServer::DealWrite_(HttpConn* client) {
    assert(client);
    ExtentTime_(client);
    threadpool_->AddTask(std::bind(&WebServer::OnWrite_, this, client));
}

void WebServer::ExtentTime_(HttpConn* client) {
    assert(client);
    if(timeoutMS_ > 0) { timer_->adjust(client->GetFd(), timeoutMS_); }
}

void WebServer::OnRead_(HttpConn* client) {
    assert(client);
    int ret = -1;
    int readErrno = 0;
    ret = client->read(&readErrno); // request 内容从 fd_ 读入读缓冲区 readBuff_
    if(ret <= 0 && readErrno != EAGAIN) {
        CloseConn_(client);
        return;
    }
    onProcess_(client); // 完成解析 request,生成 response 写入写缓冲区, 将事件改为 EPOLL_OUT, 让 epoller_ 下一次检测到写事件，把写缓冲区的内容写到 fd
}

/* 处理函数：判断读入的请求报文是否完整，决定是继续监听读还是监听写 */
// 如果请求不完整，继续读，如果请求完整，则根据请求内容生成相应的响应报文，并发送
void WebServer::onProcess_(HttpConn* client) {
    if(client->process()) {
        epoller_->ModFd(client->GetFd(), connEvent_ | EPOLLOUT);
    } else {
        epoller_->ModFd(client->GetFd(), connEvent_ | EPOLLIN);
    }
}

void WebServer::OnWrite_(HttpConn* client) {
    assert(client);
    int ret = -1;
    int writeErrno = 0;
    ret = client->write(&writeErrno);
    if(client->ToWriteBytes() == 0) {
        //发送完毕
        if(client->IsKeepAlive()) {
            onProcess_(client);
            return;
        }
    }
    else if(ret < 0) {
        //缓存满导致的，继续监听写
        if(writeErrno == EAGAIN) { // EAGAIN: try again 再次尝试传输
            epoller_->ModFd(client->GetFd(), connEvent_ | EPOLLOUT);
            return;
        }
    }
    //其他原因导致，关闭连接
    CloseConn_(client);
}

/* Create listenFd */
bool WebServer::InitSocket_() {
    int ret;
    struct sockaddr_in addr;
    if(port_ > 65535 || port_ < 1024) {
        LOG_ERROR("Port:%d error!",  port_);
        return false;
    }
    /* 配置服务器网络地址结构体 */
    addr.sin_family = AF_INET; // 使用 IPv4 协议
    addr.sin_addr.s_addr = htonl(INADDR_ANY); // 服务器可以接受来自任意本地网络接口的连接请求，即绑定到所有可用的本地 IP 地址上
    addr.sin_port = htons(port_); // 端口号设置

    struct linger optLinger = { 0 };
    if(openLinger_) {
        /* 优雅关闭: 直到所剩数据发送完毕或超时 */
        optLinger.l_onoff = 1; // 启用优雅关闭
        optLinger.l_linger = 1; // 关闭套接字时，如果还有未发送的数据，会等待 1 秒时间让数据发送完毕或者超时后再关闭，避免数据丢失，实现优雅关闭
    }

    listenFd_ = socket(AF_INET, SOCK_STREAM, 0); // 创建 socket
    if(listenFd_ < 0) {
        LOG_ERROR("Create socket error!", port_);
        return false;
    }

    ret = setsockopt(listenFd_, SOL_SOCKET, SO_LINGER, &optLinger, sizeof(optLinger)); // 设置 socket 的 SO_LINGER 选项
    if(ret < 0) {
        close(listenFd_);
        LOG_ERROR("Init linger error!", port_);
        return false;
    }

    int optval = 1;
    /* 端口复用 */
    /* 允许在服务器重启等情况下，即使之前绑定的端口处于 TIME_WAIT 状态，新的套接字也可以复用该端口进行绑定，方便服务器快速重启等情况
       note: 多个套接字绑定到单个端口时，只有最后一个套接字会正常接收数据。 */
    ret = setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, (const void*)&optval, sizeof(int));
    if(ret == -1) {
        LOG_ERROR("set socket port multiplex error !");
        close(listenFd_);
        return false;
    }

    ret = bind(listenFd_, (struct sockaddr *)&addr, sizeof(addr)); // 将创建好的 socket(listenFd_) 与特定的本地网络地址(ip:port)进行绑定
    if(ret < 0) {
        LOG_ERROR("Bind Port:%d error!", port_);
        close(listenFd_);
        return false;
    }

    ret = listen(listenFd_, 6); // 创建一个监听队列用于存放待处理的客户连接，最多有6个客户端请求处于等待被接受的队列中
    if(ret < 0) {
        LOG_ERROR("Listen port:%d error!", port_);
        close(listenFd_);
        return false;
    }
    ret = epoller_->AddFd(listenFd_,  listenEvent_ | EPOLLIN); // 将监听套接字添加到 epoller_ 事件监听集合中，同时传入 listenEvent_ | EPOLLIN 作为要关注的事件
    if(ret == 0) {
        LOG_ERROR("Add listen error!");
        close(listenFd_);
        return false;
    }
    SetFdNonblock(listenFd_); // 将监听套接字设置为非阻塞状态
    LOG_INFO("Server port:%d", port_);
    return true;
}

int WebServer::SetFdNonblock(int fd) {
    assert(fd > 0);
    // fcntl(fd, F_GETFD, 0) 获取当前文件描述符的状态标志
    // fcntl(fd, F_SETFL, xxx) 设置文件描述符的状态标志
    return fcntl(fd, F_SETFL, fcntl(fd, F_GETFD, 0) | O_NONBLOCK);
}


