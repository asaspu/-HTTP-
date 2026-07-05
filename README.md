📖 项目介绍

本项目基于 Linux ucontext 接口自研用户态协程调度框架，结合 epoll IO 多路复用、sendfile 零拷贝、timerfd 内核定时器实现高并发静态 HTTP 服务。
采用多线程架构，每个线程拥有独立协程调度器，全程无锁设计，充分利用多核 CPU；以同步代码风格实现异步非阻塞 IO，规避回调嵌套，逻辑清晰易维护。

✨ 核心功能

🧵 协程调度模块

基于 ucontext 完成协程上下文切换，每个协程分配 64KB 独立栈

协程状态划分：就绪、等待读、等待写、已销毁

epoll EPOLLONESHOT 驱动 IO 事件，IO 阻塞自动挂起、就绪自动唤醒协程

就绪链表批量调度，无线程锁竞争开销

⚡ 网络性能优化

sendfile 零拷贝传输文件，减少用户 / 内核缓冲区数据拷贝

timerfd 内核定时器统一管理长连接超时，无需用户态轮询

SO_REUSEPORT 端口复用，多线程均衡接收客户端连接

全部 Socket 设置非阻塞，封装协程版读写接口

🌐 HTTP 服务能力

支持 HTTP/1.1 GET、HEAD 请求，兼容 Keep-Alive 长连接

根据文件后缀自动匹配对应 MIME 资源类型

访问根路径自动映射 index.html 首页

过滤路径中的 ..，防御目录遍历攻击

自动返回 400/404/405 标准错误页面

🚀 编译与运行

1. 编译程序
   mkdir build
   cd build
   cmake ..
   make -j4
   
2. 准备静态资源
   在 build 目录内运行
   
   mkdir www
   
   echo '<h1>协程HTTP服务器测试页面</h1>' > www/index.html
   
   ./coro-httpd
   
4. 访问服务
5. 
   开另一个终端：./telnet 127.0.0.1 8080
   
📌 可拓展开发方向

增加 POST 请求处理，实现简单动态接口

添加文件内存缓存，减少频繁磁盘 IO

接入 OpenSSL 实现 HTTPS 加密传输

增加分级日志输出（INFO / WARN / ERROR）

捕获 SIGINT 信号，实现程序优雅退出
