#ifndef HTTP_REQUEST_H
#define HTTP_REQUEST_H

#include <unordered_map>
#include <unordered_set>
#include <string>
#include <regex>
#include <errno.h>   
#include <mysql/mysql.h>  //mysql

#include <fstream>
#include <stdio.h>
#include <sys/types.h>
#include <dirent.h>
#include <json/json.h>

#include "../buffer/buffer.h"
#include "../log/log.h"
#include "../pool/sqlconnpool.h"
#include "../pool/sqlconnRAII.h"

enum PARSE_STATE {
    REQUEST_LINE,
    HEADERS,
    BODY,
    FINISH,        
};

enum HTTP_CODE {
    NO_REQUEST = 0,
    GET_REQUEST,
    BAD_REQUEST,
    NO_RESOURSE,
    FORBIDDENT_REQUEST,
    FILE_REQUEST,
    INTERNAL_ERROR,
    CLOSED_CONNECTION,
};

class HttpRequest {
public:
    HttpRequest() { Init(); }
    ~HttpRequest() = default;

    void Init();
    HTTP_CODE parse(Buffer& buff);

    std::string path() const;
    std::string& path();
    std::string method() const;
    std::string version() const;
    std::string GetPost(const std::string& key) const;
    std::string GetPost(const char* key) const;

    bool IsKeepAlive() const;

    /* 
    todo 
    void HttpConn::ParseFormData() {}
    void HttpConn::ParseJson() {}
    */

private:
    HTTP_CODE ParseRequestLine_(const std::string& line);
    HTTP_CODE ParseHeader_(const std::string& line);
    HTTP_CODE ParseBody_();

    void ParsePath_();
    void ParseFromUrlencoded_();
    void ParseMultipartFormData_();

    static bool UserVerify(const std::string& name, const std::string& pwd, bool isLogin);
    static int ConverHex(char ch);

    std::vector<std::string> getDownloadFiles(std::string dir);
    void writeJson(std::string file, Json::Value root);
    std::string UrlDecode(const std::string& str);

    size_t contentLen;
    PARSE_STATE state_;
    std::string method_, path_, version_, body_;
    std::unordered_map<std::string, std::string> header_;
    std::unordered_map<std::string, std::string> post_;
    std::unordered_map<std::string, std::string> fileInfo;

    static const std::unordered_set<std::string> DEFAULT_HTML;
    static const std::unordered_map<std::string, int> DEFAULT_HTML_TAG;

};

#endif //HTTP_REQUEST_H