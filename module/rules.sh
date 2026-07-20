# format_cpu_ranges usage:
# $(format_cpu_ranges "$e_core")          means efficiency cores
# $(format_cpu_ranges "$p_core")          means performance cores
# $(format_cpu_ranges "$hp_core")         means high-performance cores
# Values can be combined, for example:
# $(format_cpu_ranges "$e_core $p_core")  means efficiency and performance cores
# $(format_cpu_ranges "$p_core $hp_core") means performance and high-performance cores

common_rules="
# Bind WeChat rendering and main threads to performance cores
com.tencent.mm=$(format_cpu_ranges "$e_core $p_core")
com.tencent.mm{RenderThread}=$(format_cpu_ranges "$hp_core")
com.tencent.mm{com.tencent.mm}=$(format_cpu_ranges "$p_core $hp_core")

# Bind the WeChat push process to efficiency cores
com.tencent.mm:push=$(format_cpu_ranges "$e_core")

# Bind QQ main and rendering threads to performance cores
com.tencent.mobileqq{encent.mobileqq}=$(format_cpu_ranges "$p_core $hp_core")
com.tencent.mobileqq{RenderThread}=$(format_cpu_ranges "$hp_core")

# Bind the QQ push process to efficiency cores
com.tencent.mobileqq:MSF=$(format_cpu_ranges "$e_core")

# Bind Taobao main and rendering threads to high-performance cores
com.taobao.taobao{m.taobao.taobao}=$(format_cpu_ranges "$hp_core")
com.taobao.taobao{RenderThread}=$(format_cpu_ranges "$p_core $hp_core")

# Bind Coolapk rendering and main threads to high-performance cores
com.coolapk.market{RenderThread}=$(format_cpu_ranges "$hp_core")
com.coolapk.market{.coolapk.market}=$(format_cpu_ranges "$p_core $hp_core")

# Bind Douyin critical threads to performance cores
com.ss.android.ugc.aweme{main}=$(format_cpu_ranges "$hp_core")
com.ss.android.ugc.aweme{RenderThread}=$(format_cpu_ranges "$hp_core")
com.ss.android.ugc.aweme{droid.ugc.aweme}=$(format_cpu_ranges "$p_core $hp_core")

# Delay Alipay rendering by 100 seconds and main/scanner threads by 10 seconds
com.eg.android.AlipayGphone{RenderThread}:1000=$(format_cpu_ranges "$hp_core")
com.eg.android.AlipayGphone{id.AlipayGphone}:100=$(format_cpu_ranges "$p_core $hp_core")
com.eg.android.AlipayGphone{ScanRecognize}:100=$(format_cpu_ranges "$hp_core")

# Bind Amap rendering and main threads to performance cores
com.autonavi.minimap{RenderThread}=$(format_cpu_ranges "$hp_core")
com.autonavi.minimap{utonavi.minimap}=$(format_cpu_ranges "$p_core $hp_core")

# Bind SurfaceFlinger RenderEngine to high-performance cores
surfaceflinger{RenderEngine}=$(format_cpu_ranges "$hp_core")

# Allow SurfaceFlinger to use all CPU cores
surfaceflinger=$all_core

# Bind System UI rendering and main threads to performance cores
com.android.systemui{RenderThread}=$(format_cpu_ranges "$hp_core")
com.android.systemui{ndroid.systemui}=$(format_cpu_ranges "$p_core $hp_core")
"

game_rules="
# Honor of Kings
com.tencent.tmgp.sgame{UnityMain}=$(format_cpu_ranges "$hp_core")
com.tencent.tmgp.sgame{UnityGfxDeviceW}=$(format_cpu_ranges "$p_core $hp_core")
com.tencent.tmgp.sgame=$(format_cpu_ranges "$e_core $p_core")

# Peacekeeper Elite
com.tencent.tmgp.pubgmhd{Thread-[0-9]?}=$(format_cpu_ranges "$hp_core")
com.tencent.tmgp.pubgmhd{Thread-?}=$(format_cpu_ranges "$hp_core")
com.tencent.tmgp.pubgmhd{RenderThread*}=$(format_cpu_ranges "$p_core $hp_core")
com.tencent.tmgp.pubgmhd{RHIThread}=$(format_cpu_ranges "$e_core $p_core")
com.tencent.tmgp.pubgmhd=$(format_cpu_ranges "$e_core $p_core")

# Eggy Party
com.netease.party{MainThread}=$(format_cpu_ranges "$hp_core")
com.netease.party{Compute*}=$(format_cpu_ranges "$p_core $hp_core")
com.netease.party=$(format_cpu_ranges "$e_core $p_core")

# Genshin Impact
com.miHoYo.Yuanshen{UnityMain}=$(format_cpu_ranges "$hp_core")
com.miHoYo.Yuanshen{UnityGfx*}=$(format_cpu_ranges "$p_core $hp_core")
com.miHoYo.Yuanshen=$(format_cpu_ranges "$e_core $p_core")

# Delta Force
com.tencent.tmgp.dfm{GameThread}=$(format_cpu_ranges "$hp_core")
com.tencent.tmgp.dfm{Thread*}=$(format_cpu_ranges "$p_core $hp_core")
com.tencent.tmgp.dfm{TaskGraphNP*}=$(format_cpu_ranges "$p_core $hp_core")
com.tencent.tmgp.dfm{AudioTrack}=$(format_cpu_ranges "$e_core")
com.tencent.tmgp.dfm=$(format_cpu_ranges "$e_core $p_core")

# Golden Spatula
com.tencent.jkchess{UnityMain}=$(format_cpu_ranges "$hp_core")
com.tencent.jkchess{UnityGfx*}=$(format_cpu_ranges "$p_core $hp_core")
com.tencent.jkchess=$(format_cpu_ranges "$e_core $p_core")

# Identity V
com.netease.dwrg{Thread-*}=$(format_cpu_ranges "$p_core $hp_core")
com.netease.dwrg{NativeThread}=$(format_cpu_ranges "$p_core $hp_core")
com.netease.dwrg=$(format_cpu_ranges "$e_core $p_core")
"

printf '%s\n' "$common_rules" >> "$MODPATH/applist.conf"
printf '%s\n' "$game_rules" >> "$MODPATH/applist.conf"
