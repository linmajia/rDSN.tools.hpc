/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015 Microsoft Corporation
 *
 * -=- Robust Distributed System Nucleus (rDSN) -=-
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/*
 * Description:
 *     What is this file about?
 *
 * Revision history:
 *     xxxx-xx-xx, author, first version
 *     xxxx-xx-xx, author, fix bug about xxx
 */


# include "hpc_tail_logger.h"
# include <dsn/utility/singleton_store.h>
# include <dsn/cpp/utils.h>
# include <dsn/tool-api/command.h>
# include <cstdio>
# include <cstdarg>
# include <sstream>
# include <fstream>
# include <iostream>
# include <cstring>

namespace dsn
{
    namespace tools
    {
        struct tail_log_hdr
        {
            uint32_t log_break; // '\0'
            uint32_t magic;
            int32_t  length;
            uint64_t ts;
            tail_log_hdr* prev;

            bool is_valid() { return magic == 0xdeadbeef; }
        };

        struct __tail_log_info__
        {
            uint32_t magic;
            char*    buffer;
            char*    next_write_ptr;
            tail_log_hdr *last_hdr;
        };

        typedef ::dsn::utils::safe_singleton_store<int, struct __tail_log_info__*> tail_log_manager;

        static __thread struct __tail_log_info__ s_tail_log_info;

        namespace
        {
            char* allocate_tail_log_buffer(int bytes)
            {
                char* buffer = (char*)malloc(bytes);
                if (buffer == nullptr)
                {
                    std::fprintf(stderr, "hpc_tail_logger: failed to allocate %d bytes for log buffer\n", bytes);
                }
                return buffer;
            }

            void reset_tail_log_info(__tail_log_info__* log)
            {
                log->magic = 0;
                log->buffer = nullptr;
                log->next_write_ptr = nullptr;
                log->last_hdr = nullptr;
            }

            int append_tail_log(char*& ptr, size_t& capacity, const char* fmt, ...)
            {
                if (capacity <= sizeof(tail_log_hdr))
                    return 0;

                size_t writable = capacity - sizeof(tail_log_hdr);
                va_list args;
                va_start(args, fmt);
                int written = std::vsnprintf(ptr, writable + 1, fmt, args);
                va_end(args);
                if (written < 0)
                    return written;

                size_t consumed = static_cast<size_t>(written);
                if (consumed > writable)
                    consumed = writable;

                ptr += consumed;
                capacity -= consumed;
                return static_cast<int>(consumed);
            }
        }

        hpc_tail_logger::hpc_tail_logger(const char* log_dir, logging_provider* inner)
            : logging_provider(log_dir, inner)
        {
            _log_dir = std::string(log_dir);
            _per_thread_buffer_bytes = (int)dsn_config_get_value_uint64(
                "tools.hpc_tail_logger",
                "per_thread_buffer_bytes",
                10*1024*1024, // 10 MB by default
                "buffer size for per-thread logging"
                );

            static bool register_it = false;
            if (register_it)
            {
                return;
            }

            register_it = true;

            // register command for tail logging
            ::dsn::register_command("tail-log",
                "tail-log keyword back-seconds [back-start-seconds = 0] [tid1,tid2,...]",
                "tail-log find logs with given keyword and within [now - back-seconds, now - back-start-seconds]",
                [this](const safe_vector<safe_string>& args)
                {
                    if (args.size() < 2)
                    {
                        return safe_string("invalid arguments for tail-log command");
                    }
                    else
                    {
                        std::unordered_set<int> target_threads;
                        if (args.size() >= 4)
                        {
                            std::list<std::string> tids;
                            ::dsn::utils::split_args(args[3].c_str(), tids, ',');
                            for (auto& t : tids)
                            {
                                unsigned int tid = 0;
                                if (!::dsn::utils::lexical_cast_integer<unsigned int>(t, tid))
                                {
                                    return safe_string("invalid thread id");
                                }
                                target_threads.insert((int)tid);
                            }
                        }

                        unsigned int back_seconds = 0;
                        unsigned int back_start_seconds = 0;
                        if (!::dsn::utils::lexical_cast_integer<unsigned int>(
                                std::string(args[1].c_str(), args[1].size()), back_seconds) ||
                            (args.size() >= 3 &&
                             !::dsn::utils::lexical_cast_integer<unsigned int>(
                                 std::string(args[2].c_str(), args[2].size()), back_start_seconds)))
                        {
                            return safe_string("invalid back seconds");
                        }

                        return safe_string(this->search(
                            args[0].c_str(),
                            back_seconds,
                            back_start_seconds,
                            target_threads
                            ).c_str());
                    }
                }
            );

            ::dsn::register_command("tail-log-dump",
                "tail-log-dump",
                "tail-log-dump dump all tail logs to log files",
                [this](const safe_vector<safe_string>& args)
                {
                    hpc_tail_logs_dumpper();
                    return safe_string("logs are dumped to coredurmp dir started with hpc_tail_logs.xxx.log");
                }
            );
        }

        hpc_tail_logger::~hpc_tail_logger(void)
        {
            std::vector<int> threads;
            tail_log_manager::instance().get_all_keys(threads);

            for (auto& tid : threads)
            {
                __tail_log_info__* log;
                if (!tail_log_manager::instance().get(tid, log))
                    continue;

                free(log->buffer);
                reset_tail_log_info(log);
                tail_log_manager::instance().remove(tid);
            }
        }

        void hpc_tail_logger::flush()
        {
            hpc_tail_logs_dumpper();
        }

        void hpc_tail_logger::hpc_tail_logs_dumpper()
        {
            uint64_t nts = dsn_now_ns();
            std::stringstream log;
            log << _log_dir << "/hpc_tail_logs." << nts << ".log";

            std::ofstream olog(log.str().c_str());

            std::vector<int> threads;
            tail_log_manager::instance().get_all_keys(threads);

            for (auto& tid : threads)
            {
                __tail_log_info__* log;
                if (!tail_log_manager::instance().get(tid, log))
                    continue;

                if (log->last_hdr == nullptr)
                    continue;

                tail_log_hdr *hdr = log->last_hdr, *tmp = log->last_hdr;
                do
                {
                    if (!tmp->is_valid())
                        break;

                    char* llog = (char*)(tmp)-tmp->length;
                    olog << llog << std::endl;

                    // try previous log
                    tmp = tmp->prev;

                } while (tmp != nullptr && tmp != hdr);
            }

            olog.close();
        }

        std::string hpc_tail_logger::search(
            const char* keyword,
            unsigned int back_seconds,
            unsigned int back_start_seconds,
            std::unordered_set<int>& target_threads)
        {
            uint64_t nts = dsn_now_ns();
            uint64_t start = nts - static_cast<uint64_t>(back_seconds)* 1000 * 1000 * 1000; // second to nanosecond
            uint64_t end = nts - static_cast<uint64_t>(back_start_seconds)* 1000 * 1000 * 1000; // second to nanosecond

            std::vector<int> threads;
            tail_log_manager::instance().get_all_keys(threads);

            std::stringstream ss;
            int log_count = 0;

            for (auto& tid : threads)
            {
                __tail_log_info__* log;
                if (!tail_log_manager::instance().get(tid, log))
                    continue;

                // filter by tid
                if (target_threads.size() > 0 && target_threads.find(tid) == target_threads.end())
                {
                    continue;
                }

                if (log->last_hdr == nullptr)
                    continue;

                tail_log_hdr *hdr = log->last_hdr, *tmp = log->last_hdr;
                do
                {
                    if (!tmp->is_valid())
                        break;

                    // filter by time
                    if (tmp->ts < start)
                        break;

                    if (tmp->ts > end)
                    {
                        tmp = tmp->prev;
                        continue;
                    }

                    // filter by keyword
                    char* llog = (char*)(tmp) - tmp->length;
                    if (strstr(llog, keyword))
                    {
                        ss << llog << std::endl;
                        log_count++;
                    }

                    // try previous log
                    tmp = tmp->prev;

                } while (tmp != nullptr && tmp != hdr);
            }

            char strb[24], stre[24];
            ::dsn::utils::time_ms_to_string(start / 1000000, strb, sizeof(strb));
            ::dsn::utils::time_ms_to_string(end / 1000000, stre, sizeof(stre));

            ss << "------------------------------------------" << std::endl;
            ss << "In total (" << log_count << ") log entries are found between [" << strb << ", "<< stre << "] " << std::endl;

            return ss.str();
        }

        void hpc_tail_logger::dsn_logv(const char *file,
            const char *function,
            const int line,
            dsn_log_level_t log_level,
            const char* title,
            const char *fmt,
            va_list args
            )
        {
            // init log buffer if necessary
            if (s_tail_log_info.magic != 0xdeadbeef)
            {
                s_tail_log_info.buffer = allocate_tail_log_buffer(_per_thread_buffer_bytes);
                if (s_tail_log_info.buffer == nullptr)
                    return;

                s_tail_log_info.next_write_ptr = s_tail_log_info.buffer;
                s_tail_log_info.last_hdr = nullptr;
                memset(s_tail_log_info.buffer, '\0', _per_thread_buffer_bytes);

                tail_log_manager::instance().put(::dsn::utils::get_current_tid(), &s_tail_log_info);
                s_tail_log_info.magic = 0xdeadbeef;
            }

            // get enough write space >= 1K
            if (s_tail_log_info.next_write_ptr + 1024 + sizeof(tail_log_hdr) > s_tail_log_info.buffer + _per_thread_buffer_bytes)
            {
                s_tail_log_info.next_write_ptr = s_tail_log_info.buffer;
            }
            char* ptr = s_tail_log_info.next_write_ptr;
            char* ptr0 = ptr; // remember it
            size_t capacity = static_cast<size_t>(s_tail_log_info.buffer + _per_thread_buffer_bytes - ptr);

            // print verbose log header
            uint64_t ts = 0;
            int tid = ::dsn::utils::get_current_tid();
            if (::dsn::tools::is_engine_ready())
                ts = dsn_now_ns();
            char str[24];
            ::dsn::utils::time_ms_to_string(ts / 1000000, str, sizeof(str));
            auto wn = append_tail_log(ptr, capacity, "%s (%" PRIu64 " %04x) ", str, ts, tid);
            if (wn < 0)
                return;

            auto t = task::get_current_task_id();
            if (t)
            {
                if (nullptr != task::get_current_worker2())
                {
                    wn = append_tail_log(ptr, capacity, "%6s.%7s%d.%016" PRIx64 ": ",
                        task::get_current_node_name(),
                        task::get_current_worker2()->pool_spec().name.c_str(),
                        task::get_current_worker2()->index(),
                        t
                        );
                }
                else
                {
                    wn = append_tail_log(ptr, capacity, "%6s.%7s.%05d.%016" PRIx64 ": ",
                        task::get_current_node_name(),
                        "io-thrd",
                        tid,
                        t
                        );
                }
            }
            else
            {
                wn = append_tail_log(ptr, capacity, "%6s.%7s.%05d: ",
                    task::get_current_node_name(),
                    "io-thrd",
                    tid
                    );
            }
            if (wn < 0)
                return;

            // print body
            size_t writable = capacity > sizeof(tail_log_hdr) ? capacity - sizeof(tail_log_hdr) : 0;
            wn = writable > 0 ? std::vsnprintf(ptr, writable + 1, fmt, args) : 0;
            if (wn < 0)
            {
                wn = append_tail_log(ptr, capacity, "-- cannot printf due to that log entry has error ---");
                if (wn < 0)
                    return;
            }
            else
            {
                size_t consumed = static_cast<size_t>(wn);
                if (consumed > writable)
                    consumed = writable;

                ptr += consumed;
                capacity -= consumed;
            }

            // set binary entry header on tail
            tail_log_hdr* hdr = (tail_log_hdr*)ptr;
            hdr->log_break = 0;
            hdr->length = 0;
            hdr->magic = 0xdeadbeef;
            hdr->ts = ts;
            hdr->length = static_cast<int>(ptr - ptr0);
            hdr->prev = s_tail_log_info.last_hdr;
            s_tail_log_info.last_hdr = hdr;

            ptr += sizeof(tail_log_hdr);
            capacity -= sizeof(tail_log_hdr);

            // set next write ptr
            s_tail_log_info.next_write_ptr = ptr;

            // dump critical logs on screen
            if (log_level >= LOG_LEVEL_WARNING)
            {
                std::cout << ptr0 << std::endl;
            }
        }
    }
}
