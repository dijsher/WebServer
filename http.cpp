#include <cstdio>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <cerrno>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/wait.h>

// C++标准库
#include <iostream>
#include <string>
#include <thread>
#include <fstream>
#include <system_error>


const std::string SERVER_STRING = "Server:  http/0.1.0\r\n";

// 函数声明（C++风格）
void accept_request(int client);
void bad_request(int client);
void cat(int client, std::ifstream& resource);
void cannot_execute(int client);
void error_die(const std::string& msg);
void execute_cgi(int client, const std::string& path, const std::string& method, const std::string& query_string);
int get_line(int sock, char* buf, int size);
void headers(int client, const std::string& filename);
void not_found(int client);
void serve_file(int client, const std::string& filename);
int startup(u_short* port);
void unimplemented(int client);


// 处理客户端请求（C++线程函数，直接接收int参数）
void accept_request(int client) {
    char buf[1024];
    int numchars;
    std::string method;  
    std::string url;
    std::string path;
    size_t i, j;
    struct stat st;
    bool cgi = false;    
    std::string query_string;

    // 读取请求行
    numchars = get_line(client, buf, sizeof(buf));
    if (numchars <= 0) {
        close(client);
        return;
    }

    // 提取请求方法（GET/POST）
    i = 0;
    j = 0;
    while (!isspace(static_cast<unsigned char>(buf[j])) && i < 255) { 
        method += buf[j];  // 字符串拼接（避免缓冲区溢出）
        i++;
        j++;
    }

    // 检查支持的方法
    if (method != "GET" && method != "POST") {
        unimplemented(client);
        close(client);
        return;
    }
    if (method == "POST") {
        cgi = true;
    }

    // 跳过空格，提取URL
    while (isspace(static_cast<unsigned char>(buf[j])) && j < sizeof(buf)) {
        j++;
    }
    i = 0;
    while (!isspace(static_cast<unsigned char>(buf[j])) && i < 255 && j < sizeof(buf)) {
        url += buf[j];
        i++;
        j++;
    }

    // 处理GET请求的查询参数
    if (method == "GET") {
        size_t q_pos = url.find('?');
        if (q_pos != std::string::npos) {
            cgi = true;
            query_string = url.substr(q_pos + 1);  // 提取?后的参数
            url = url.substr(0, q_pos);            // 截断URL
        }
    }

    // 构建文件路径
    path = "httpdocs" + url;
    if (!path.empty() && path.back() == '/') {
        path += "test.html";
    }

    // 检查文件是否存在
    if (stat(path.c_str(), &st) == -1) {  // c_str()转换为C风格字符串
        // 跳过剩余请求头
        while (numchars > 0 && std::string(buf) != "\n") {
            numchars = get_line(client, buf, sizeof(buf));
        }
        not_found(client);
    } else {
        // 处理目录请求（自动追加index.html）
        if ((st.st_mode & S_IFMT) == S_IFDIR) {
            path += "/test.html";
        }

        // 检查文件是否可执行（需启用CGI）
        if ((st.st_mode & S_IXUSR) || (st.st_mode & S_IXGRP) || (st.st_mode & S_IXOTH)) {
            cgi = true;
        }

        // 处理静态文件或CGI
        if (!cgi) {
            serve_file(client, path);
        } else {
            execute_cgi(client, path, method, query_string);
        }
    }

    close(client);
}


// 400错误响应
void bad_request(int client) {
    std::string response;
    response += "HTTP/1.0 400 BAD REQUEST\r\n";
    response += SERVER_STRING;
    response += "Content-type: text/html\r\n";
    response += "\r\n";
    response += "<P>Your browser sent a bad request, such as a POST without a Content-Length.\r\n";
    send(client, response.c_str(), response.size(), 0);  // 用string的size()避免长度计算错误
}


// 发送文件内容（C++文件流版本）
void cat(int client, std::ifstream& resource) {
    char buf[1024];
    while (resource.read(buf, sizeof(buf))) {  // 用read读取二进制数据
        send(client, buf, resource.gcount(), 0);  // gcount()获取实际读取字节数
    }
    // 处理剩余数据
    if (resource.gcount() > 0) {
        send(client, buf, resource.gcount(), 0);
    }
}


// 500错误响应（CGI执行失败）
void cannot_execute(int client) {
    std::string response;
    response += "HTTP/1.0 500 Internal Server Error\r\n";
    response += SERVER_STRING;
    response += "Content-type: text/html\r\n";
    response += "\r\n";
    response += "<P>Error prohibited CGI execution.\r\n";
    send(client, response.c_str(), response.size(), 0);
}


// 错误处理（C++风格输出）
void error_die(const std::string& msg) {
    std::cerr << msg << ": " << std::strerror(errno) << std::endl;  // 结合cerr和错误码
    std::exit(1);
}


// 执行CGI脚本
void execute_cgi(int client, const std::string& path, const std::string& method, const std::string& query_string) {
    char buf[1024];
    int cgi_output[2];
    int cgi_input[2];
    pid_t pid;
    int status;
    char c;
    int numchars = 1;
    int content_length = -1;

    // 读取请求头（GET跳过，POST获取Content-Length）
    if (method == "GET") {
        while (numchars > 0 && std::string(buf) != "\n") {
            numchars = get_line(client, buf, sizeof(buf));
        }
    } else {  // POST
        numchars = get_line(client, buf, sizeof(buf));
        while (numchars > 0 && std::string(buf) != "\n") {
            if (std::string(buf, 0, 15) == "Content-Length:") {
                content_length = std::atoi(buf + 16);
            }
            numchars = get_line(client, buf, sizeof(buf));
        }
        if (content_length == -1) {
            bad_request(client);
            return;
        }
    }

    // 发送200响应头
    std::string response = "HTTP/1.0 200 OK\r\n";
    send(client, response.c_str(), response.size(), 0);

    // 创建管道
    if (pipe(cgi_output) < 0 || pipe(cgi_input) < 0) {
        cannot_execute(client);
        return;
    }

    // 创建子进程执行CGI
    if ((pid = fork()) < 0) {
        cannot_execute(client);
        return;
    }

    if (pid == 0) {  // 子进程
        // 重定向标准输入输出
        dup2(cgi_output[1], STDOUT_FILENO);
        dup2(cgi_input[0], STDIN_FILENO);
        close(cgi_output[0]);
        close(cgi_input[1]);

        // 设置环境变量
        setenv("REQUEST_METHOD", method.c_str(), 1);
        if (method == "GET") {
            setenv("QUERY_STRING", query_string.c_str(), 1);
        } else {  // POST
            std::string len_str = std::to_string(content_length);  
            setenv("CONTENT_LENGTH", len_str.c_str(), 1);
        }

        // 执行CGI脚本
        execl(path.c_str(), path.c_str(), nullptr);  
        std::exit(0);
    } else {  // 父进程
        close(cgi_output[1]);
        close(cgi_input[0]);

        // 处理POST数据（写入CGI输入）
        if (method == "POST") {
            for (int i = 0; i < content_length; i++) {
                recv(client, &c, 1, 0);
                write(cgi_input[1], &c, 1);
            }
        }

        // 读取CGI输出并发送给客户端
        while (read(cgi_output[0], &c, 1) > 0) {
            send(client, &c, 1, 0);
        }

        // 清理资源
        close(cgi_output[0]);
        close(cgi_input[1]);
        waitpid(pid, &status, 0);
    }
}


// 读取HTTP请求行（保留char缓冲区，兼容底层API）
int get_line(int sock, char* buf, int size) {
    int i = 0;
    char c = '\0';
    int n;

    while (i < size - 1 && c != '\n') {
        n = recv(sock, &c, 1, 0);
        if (n > 0) {
            if (c == '\r') {
                n = recv(sock, &c, 1, MSG_PEEK);  // 预读检查是否为\n
                if (n > 0 && c == '\n') {
                    recv(sock, &c, 1, 0);  //  consume \n
                } else {
                    c = '\n';  // 单独的\r视为换行
                }
            }
            buf[i] = c;
            i++;
        } else {
            c = '\n';  // 读取失败终止
        }
    }
    buf[i] = '\0';
    return i;
}


// 发送HTTP响应头
void headers(int client, const std::string& filename) {
    std::string response;
    response += "HTTP/1.0 200 OK\r\n";
    response += SERVER_STRING;
    response += "Content-Type: text/html\r\n";  // 简化处理，实际应根据文件类型动态设置
    response += "\r\n";
    send(client, response.c_str(), response.size(), 0);
}


// 404错误响应
void not_found(int client) {
    std::string response;
    response += "HTTP/1.0 404 NOT FOUND\r\n";
    response += SERVER_STRING;
    response += "Content-Type: text/html\r\n";
    response += "\r\n";
    response += "<HTML><TITLE>Not Found</TITLE>\r\n";
    response += "<BODY><P>The server could not fulfill your request because the resource specified is unavailable or nonexistent.\r\n";
    response += "</BODY></HTML>\r\n";
    send(client, response.c_str(), response.size(), 0);
}


// 处理静态文件
void serve_file(int client, const std::string& filename) {
    std::ifstream resource(filename, std::ios::binary);  // 二进制模式打开，避免文本转换
    char buf[1024];
    int numchars = 1;

    // 跳过请求头
    while (numchars > 0 && std::string(buf) != "\n") {
        numchars = get_line(client, buf, sizeof(buf));
    }

    if (!resource.is_open()) {  // 检查文件是否打开成功
        not_found(client);
    } else {
        headers(client, filename);
        cat(client, resource);  // 发送文件内容
    }
    // ifstream自动关闭，无需手动fclose
}


// 启动服务器（创建socket、绑定、监听）
int startup(u_short* port) {
    int httpd = socket(PF_INET, SOCK_STREAM, 0);
    if (httpd == -1) {
        error_die("socket creation failed");
    }

    // 设置端口复用
    int option = 1;
    socklen_t optlen = sizeof(option);
    if (setsockopt(httpd, SOL_SOCKET, SO_REUSEADDR, &option, optlen) == -1) {
        error_die("setsockopt failed");
    }

    // 绑定地址和端口
    struct sockaddr_in name;
    std::memset(&name, 0, sizeof(name));
    name.sin_family = AF_INET;
    name.sin_port = htons(*port);
    name.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(httpd, reinterpret_cast<struct sockaddr*>(&name), sizeof(name)) < 0) {  // C++风格强制转换
        error_die("bind failed");
    }

    // 动态分配端口（如果port为0）
    if (*port == 0) {
        socklen_t namelen = sizeof(name);
        if (getsockname(httpd, reinterpret_cast<struct sockaddr*>(&name), &namelen) == -1) {
            error_die("getsockname failed");
        }
        *port = ntohs(name.sin_port);
    }

    // 开始监听
    if (listen(httpd, 5) < 0) {
        error_die("listen failed");
    }

    return httpd;
}


// 501错误响应（方法未实现）
void unimplemented(int client) {
    std::string response;
    response += "HTTP/1.0 501 Method Not Implemented\r\n";
    response += SERVER_STRING;
    response += "Content-Type: text/html\r\n";
    response += "\r\n";
    response += "<HTML><HEAD><TITLE>Method Not Implemented</TITLE></HEAD>\r\n";
    response += "<BODY><P>HTTP request method not supported.</P></BODY></HTML>\r\n";
    send(client, response.c_str(), response.size(), 0);
}


// 主函数
int main() {
    int server_sock = -1;
    u_short port = 6379;
    int client_sock = -1;
    struct sockaddr_in client_name;
    socklen_t client_name_len = sizeof(client_name);

    // 启动服务器
    server_sock = startup(&port);
    std::cout << "HTTP server socket: " << server_sock << std::endl;
    std::cout << "Server running on port: " << port << std::endl;

    // 循环接收客户端连接
    while (true) {
        client_sock = accept(server_sock,
                            reinterpret_cast<struct sockaddr*>(&client_name),
                            &client_name_len);
        if (client_sock == -1) {
            error_die("accept failed");
        }

        // 打印客户端信息
        std::cout << "New connection - IP: " << inet_ntoa(client_name.sin_addr)
                  << ", Port: " << ntohs(client_name.sin_port) << std::endl;

        // 用C++线程处理请求（分离线程避免资源泄漏）
        std::thread(&accept_request, client_sock).detach();
    }

    close(server_sock);
    return 0;
}