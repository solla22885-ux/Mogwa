namespace m1
{
    namespace util
    {
        template<class F>
        class scope_exit {
            F func;
        public:
            explicit scope_exit(F&& f) noexcept : func(std::forward<F>(f)) {}
            ~scope_exit() noexcept { func(); }
            scope_exit(const scope_exit&) = delete;
            scope_exit& operator=(const scope_exit&) = delete;
        };
    }
}