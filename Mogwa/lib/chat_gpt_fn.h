// chat_gpt_fn.h : 헤더 파일.
//

#pragma once

///////////////////////////////////////////////////////////////////////////////

namespace m1 {
    namespace gpt_fn {
        inline constexpr std::string_view CHAT_GPT_PATH = "https://api.openai.com/v1/chat/completions";
        class gpt_bot {
        public:
            gpt_bot() {};
            ~gpt_bot() {};

            std::string model;
            std::string api_key;
            std::vector<std::string> messages;
        public:
            bool execute(std::string& output);
        };
    }
}
