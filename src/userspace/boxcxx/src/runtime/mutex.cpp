
namespace {
thread_local unsigned char g_thread_anchor;
}

extern "C" void *__boxcxx_thread_id() noexcept
{
    return &g_thread_anchor;
}