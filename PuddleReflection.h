#pragma once

// Refleksi puddle yang dijalankan di RENDER THREAD (thread pemilik EGL context).
// Game thread hanya mem-publish data puddle; semua panggilan GL ada di hook.
namespace PuddleReflection
{
    constexpr int GRID = 9; // 9x9 titik = 8x8 sel; permukaan air mengikuti tinggi tanah

    struct Item
    {
        float cx, cy;          // pusat (world)
        float ax, ay;          // setengah-vektor sumbu "u" (world XY)
        float sx, sy;          // setengah-vektor sumbu "v" (world XY)
        float z[GRID * GRID];     // tinggi permukaan per titik grid (indeks j*GRID+i)
        float valid[GRID * GRID]; // 1 = menempel di tanah, 0 = tertutup dinding/tepi (di-fade)
        float strength;        // 0..1 (fade umur puddle)
        float seed[4];         // acak 0..1 -> bentuk blob unik tiap puddle
    };

    struct Env
    {
        float skyTop[3];
        float skyHor[3];
        float light;   // 0..1 kecerahan siang/malam
        float cloud;   // 0..1 tutupan awan
    };

    constexpr int MAX_ITEMS = 4;

    void Install(float wallDistance);
    void Shutdown();
    // (Tidak dipakai lagi: bentuk dibuat prosedural di shader.) Dibiarkan agar API lama tetap valid.
    void SetMask(const unsigned char* data, int width, int height);
    // Tulis 4 piksel penanda ke baris pertama RGBA8888 supaya upload tekstur puddle
    // bisa dikenali di render thread (alpha=1 -> tidak terlihat).
    void StampMagic(unsigned char* rgbaRow0);
    void Publish(const Item* items, int count, float timeSeconds, const Env& env);

    // True bila pass refleksi benar-benar berjalan dalam ~1 detik terakhir.
    bool IsActive();
}
