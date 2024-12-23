#include "httprequest.h"
using namespace std;
const char CRLF[] = "\r\n"; // 结束符：回车、换行

const unordered_set<string> HttpRequest::DEFAULT_HTML{
            "/index", "/register", "/login",
             "/welcome", "/video", "/picture",
             "/upload", "/download", "/live_stream", };

const unordered_map<string, int> HttpRequest::DEFAULT_HTML_TAG {
            {"/register.html", 0}, {"/login.html", 1},  };

void HttpRequest::Init() {
    method_ = path_ = version_ = body_ = "";
    state_ = REQUEST_LINE;
    contentLen = 0;
    header_.clear();
    post_.clear();
}

bool HttpRequest::IsKeepAlive() const {
    if(header_.count("Connection") == 1) {
        return header_.find("Connection")->second == "keep-alive" && version_ == "1.1";
    }
    return false;
}

HTTP_CODE HttpRequest::parse(Buffer& buff) {
    
    // 外部 process() 函数调用时，确保 buff.ReadableBytes() > 0
    while(buff.ReadableBytes()) {
        const char* lineEnd;
        std::string line;

        // 除了消息体外，逐行解析
        if (state_ != BODY) {
            // 从buffer中读指针开始到写指针结束（前闭后开），并去除"\r\n"，返回有效数据的行末指针，search 函数在没找到子序列的时候会直接返回末尾的地址
            lineEnd = search(buff.Peek(), buff.BeginWriteConst(), CRLF, CRLF + 2); // 查找 [first1, last1) 范围内第一个 [first2, last2) 子序列
            //如果没找到CRLF，也不是BODY，那么一定不完整
            if (lineEnd == buff.BeginWrite() && state_ == HEADERS) return NO_REQUEST;
            line = std::string(buff.Peek(), lineEnd); 
            buff.RetrieveUntil(lineEnd + 2); // 除消息体外，都有回车换行符
        } else {
            // 消息体读取全部内容，同时清空缓存
            body_ += buff.RetrieveAllToStr();
            LOG_DEBUG("body_.size(): %d Byte, contentLen: %d Byte.", body_.size(), contentLen);
            if (body_.size() < contentLen) {
                return NO_REQUEST;
            }
        }

        switch(state_) {
            case REQUEST_LINE: {
                HTTP_CODE ret = ParseRequestLine_(line);
                if (ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                }
                ParsePath_(); // 解析 path_ 变量，主要作用是将 path_ 转换为 xxx.html
                break; // 跳出 switch
            }
            case HEADERS: {
                HTTP_CODE ret = ParseHeader_(line);
                //内部根据content-length字段判断请求完整，提前结束
                if (ret == GET_REQUEST) {
                    return GET_REQUEST;
                }
                break;
            }
            case BODY: {
                HTTP_CODE ret = ParseBody_();
                if (ret == GET_REQUEST) {
                    return GET_REQUEST;
                }
                break;
            }
            default:
                break;
        }
    }
    //缓存读空了，但请求还不完整，继续读
    return NO_REQUEST;
}

// 解析路径
const static std::regex regex_filesPath(".files.+");

vector<string> HttpRequest::getDownloadFiles(string dir) {
    vector<string> files;
    DIR *pDir = NULL;
	struct dirent * pEnt = NULL;

	pDir = opendir(dir.c_str());
	if (NULL == pDir)
	{
		perror("opendir");
		return files;
	}	
    
	while (1)
	{
		pEnt = readdir(pDir);
		if(pEnt != NULL)
		{
            if(strcmp(".",pEnt->d_name)==0 || strcmp("..",pEnt->d_name)==0)
            {
                continue;
            }
			files.push_back(pEnt->d_name);
		}
		else
		{
			break;
		}
	};
	return files;
}

void HttpRequest::writeJson(string file, Json::Value root) {
    std::ostringstream os;
    Json::StreamWriterBuilder writerBuilder;
    std::unique_ptr<Json::StreamWriter> jsonWriter(writerBuilder.newStreamWriter());

    jsonWriter->write(root, &os);

    ofstream ofs;
    ofs.open(file);
    assert(ofs.is_open());
    ofs << os.str();
    ofs.close();

    return;
}

char FromHex(char x) {
	if (x >= 'A' && x <= 'Z') return x - 'A' + 10;
	else if (x >= 'a' && x <= 'z') return x - 'a' + 10;
	else if (x >= '0' && x <= '9') return x - '0';
	else assert(0);

}

// 用来解析中文文件名
string HttpRequest::UrlDecode(const string& str) {
	std::string strTemp = "";
	size_t length = str.length();
	for (size_t i = 0; i < length; i++)
	{
		if (str[i] == '+') strTemp += ' ';
		else if (str[i] == '%') {
			assert(i + 2 < length);
			char high = FromHex(str[++i]);
			char low = FromHex(str[++i]);
			strTemp += high * 16 + low;
		} else {
            strTemp += str[i];
        }
	}
	return strTemp;
}

void HttpRequest::ParsePath_() {
    if(path_ == "/") {
        path_ = "/index.html"; 
    } else if (DEFAULT_HTML.count(path_)) {
        path_ += ".html";
    } else if (path_ == "/list.json") {
        auto files = getDownloadFiles("./resources/files");
        Json::Value root;
        Json::Value file;
        for (int i = 0; i < (int)files.size(); i ++)
        {
            file["filename"] = files[i];
            root.append(file);
        }
        writeJson("./resources/list.json", root);
    } else if (regex_match(path_, regex_filesPath)) {
        string newpath = "/files/";
        string tobedecode = path_.substr(7);
        newpath += UrlDecode(tobedecode);
        path_ = newpath;
    }
}

HTTP_CODE HttpRequest::ParseRequestLine_(const string& line) {
    // 匹配正则表达式，以括号（）划分组别，一共三组分别匹配：请求方法（GET、POST）; URL 资源路径; 协议版本
    // ^ 符号表示匹配字符串的开头, $ 符号表示匹配字符串的结尾
    // [^ ]* 表示匹配零个或多个非空格字符, ^字符在此处表示取反的意思
    regex patten("^([^ ]*) ([^ ]*) HTTP/([^ ]*)$"); 
    smatch subMatch;
    if(regex_match(line, subMatch, patten)) {   
        method_ = subMatch[1];
        path_ = subMatch[2];
        version_ = subMatch[3];
        state_ = HEADERS; // 解析请求行完毕，状态置为解析请求头 HEADERS
        return NO_REQUEST; //request isn't completed
    }
    LOG_ERROR("RequestLine Error");
    return BAD_REQUEST;
}

HTTP_CODE HttpRequest::ParseHeader_(const string& line) {
    // 从起始位置（^表示匹配字符串的开头）到结束位置（$表示匹配字符串的结尾）进行匹配
    // ([^:]*表示匹配零个或多个非冒号（:）字符，用于捕获键（key）部分的文本
    // ?表示前面的空格是可选的，用于匹配可能存在或不存在的空格
    // .*表示匹配零个或多个任意字符，这个捕获组用于捕获冒号后面的值（value）部分的文本
    regex patten("^([^:]*): ?(.*)$");
    smatch subMatch;
    if(regex_match(line, subMatch, patten)) {
        header_[subMatch[1]] = subMatch[2];
        if (subMatch[1] == "Content-Length") {
            contentLen = stoi(subMatch[2]);
        }
        return NO_REQUEST;
    } else if (contentLen) {
        state_ = BODY;
        return NO_REQUEST;
    } else {
        return GET_REQUEST;
    }
}

/* 解析请求消息体，根据消息类型解析内容 */
HTTP_CODE HttpRequest::ParseBody_()
{
    //key-value
    if (method_ == "POST" && header_["Content-Type"] == "application/x-www-form-urlencoded")
    {
        ParseFromUrlencoded_();
        if(DEFAULT_HTML_TAG.count(path_)) {
            int tag = DEFAULT_HTML_TAG.find(path_)->second;
            if(tag == 0 || tag == 1) {
                bool isLogin = (tag == 1); // 登录或注册
                if(UserVerify(post_["username"], post_["password"], isLogin)) {
                    path_ = "/welcome.html";
                } 
                else {
                    path_ = "/error.html";
                }
            }
        }
    }
    else if (method_ == "POST" && header_["Content-Type"].find("multipart/form-data") != string::npos)
    {
        ParseMultipartFormData_();
        LOG_INFO("upload file!");
        ofstream ofs;
        ofs.open("./resources/response.txt", ios::ate);
        ofs << "./resources/files/" << fileInfo["filename"];
        ofs.close();
        path_ = "/response.txt";
    }
    LOG_DEBUG("Body:%s len:%d", body_.c_str(), body_.size());
    return GET_REQUEST;
}


// 十六进制转十进制
int HttpRequest::ConverHex(char ch) {
    if(ch >= 'A' && ch <= 'F') return ch -'A' + 10;
    if(ch >= 'a' && ch <= 'f') return ch -'a' + 10;
    return ch;
}

void HttpRequest::ParseMultipartFormData_() {
    if (body_.size() == 0) return;

    size_t st = 0, ed = 0;
    ed = body_.find(CRLF);
    string boundary = body_.substr(0, ed);

    // 解析文件信息
    st = body_.find("filename=\"", ed) + strlen("filename=\"");
    ed = body_.find("\"", st);
    fileInfo["filename"] = body_.substr(st, ed - st);
    
    // 解析文件内容，文件内容以\r\n\r\n开始
    st = body_.find("\r\n\r\n", ed) + strlen("\r\n\r\n");
    ed = body_.find(boundary, st) - 2; // 文件结尾也有\r\n
    string content = body_.substr(st, ed - st);

    ofstream ofs;
    // 如果文件分多次发送，应该采用app，同时为避免重复上传，应该用md5做校验
    ofs.open("./resources/files/" + fileInfo["filename"], ios::ate);
    ofs << content;
    ofs.close();
}

void HttpRequest::ParseFromUrlencoded_() {
    if(body_.size() == 0) { return; }

    string key, value;
    int num = 0;
    int n = body_.size();
    int i = 0, j = 0;

    for(; i < n; i++) {
        char ch = body_[i];
        switch (ch) {
        case '=':
            key = body_.substr(j, i - j); // 等号前为 key
            j = i + 1;
            break;
        case '+':
            body_[i] = ' '; // + 替换为空格
            break;
        case '%': // % 后面两位十六进制数替换为十进制数
            num = ConverHex(body_[i + 1]) * 16 + ConverHex(body_[i + 2]);
            body_[i + 2] = num % 10 + '0';
            body_[i + 1] = num / 10 + '0';
            i += 2;
            break;
        case '&': // & 前为 value
            value = body_.substr(j, i - j);
            j = i + 1;
            post_[key] = value; // key, value 键值对存储到 <unorderded_map>post_ 中
            LOG_DEBUG("ParseFromUrlencoded_: %s = %s", key.c_str(), value.c_str());
            break;
        default:
            break;
        }
    }
    assert(j <= i);
    if(post_.count(key) == 0 && j < i) { // 还有剩余的字符串部分可以作为值来处理
        value = body_.substr(j, i - j);
        post_[key] = value;
    }
}

bool HttpRequest::UserVerify(const string &name, const string &pwd, bool isLogin) {
    if(name == "" || pwd == "") { return false; }
    LOG_INFO("Verify name:%s pwd:%s", name.c_str(), pwd.c_str());
    MYSQL* sql;
    SqlConnRAII(&sql,  SqlConnPool::Instance());
    assert(sql);
    
    bool flag = false;
    unsigned int j = 0;
    char order[256] = { 0 };
    MYSQL_FIELD *fields = nullptr;
    MYSQL_RES *res = nullptr;
    
    if(!isLogin) { flag = true; }
    /* 查询用户及密码 */
    snprintf(order, 256, "SELECT username, passwd FROM user WHERE username='%s' LIMIT 1", name.c_str());
    LOG_DEBUG("query order: %s", order);

    if(mysql_query(sql, order)) { // 执行查询出现错误（函数返回非0值）
        mysql_free_result(res); // 遵循释放资源的规范操作
        return false; 
    }
    res = mysql_store_result(sql); // 获取查询结果集并存储到 res 指针指向的内存中
    j = mysql_num_fields(res); // 获取结果集中字段的数量
    fields = mysql_fetch_fields(res); // 获取结果集的字段信息

    while(MYSQL_ROW row = mysql_fetch_row(res)) { // 遍历查询结果集中的每一行数据（实际按逻辑应该只有一行，因为前面 LIMIT 1 限制了）
        LOG_DEBUG("MYSQL ROW: %s %s", row[0], row[1]);
        string password(row[1]);
        /* 登录行为 */
        if(isLogin) {
            if(pwd == password) { flag = true; }
            else {
                flag = false;
                LOG_DEBUG("password error!");
            }
        } 
        else { 
            flag = false; 
            LOG_DEBUG("user used!");
        }
    }
    mysql_free_result(res); // 遵循释放资源的规范操作

    /* 注册行为 且 用户名未被使用*/
    if(!isLogin && flag == true) {
        LOG_DEBUG("regirster!");
        bzero(order, 256);
        snprintf(order, 256,"INSERT INTO user(username, passwd) VALUES('%s','%s')", name.c_str(), pwd.c_str());
        LOG_DEBUG( "query order: %s", order);
        if(mysql_query(sql, order)) { 
            LOG_DEBUG( "MYSQL (user, passwd) insert error!");
            flag = false; 
        }
        flag = true;
    }
    SqlConnPool::Instance()->FreeConn(sql); // 释放 sql connection 返回 sqlconnpool
    LOG_DEBUG( "UserVerify complete !!");
    return flag;
}

std::string HttpRequest::path() const{
    return path_;
}

std::string& HttpRequest::path(){
    return path_;
}

std::string HttpRequest::method() const {
    return method_;
}

std::string HttpRequest::version() const {
    return version_;
}

std::string HttpRequest::GetPost(const std::string& key) const {
    assert(key != "");
    if(post_.count(key) == 1) {
        return post_.find(key)->second;
    }
    return "";
}

std::string HttpRequest::GetPost(const char* key) const {
    assert(key != nullptr);
    if(post_.count(key) == 1) {
        return post_.find(key)->second;
    }
    return "";
}