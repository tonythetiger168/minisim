// wave_reader.hpp on a hand-written VCD: names, ranges, aliases, padding,
// same-time and repeated values, and the same result on any thread count.
#include <cassert>
#include <cstdio>
#include <fstream>

#include "wave_reader.hpp"

int main() {
    const char* path = "test_wave.vcd";
    {
        std::ofstream f(path);
        f << "$date today $end\n$timescale 10 ps $end\n"
             "$scope module tb $end\n$var wire 1 ! clk $end\n$var reg 8 \" q [7:0] $end\n"
             "$var real 64 # r $end\n"
             "$scope module u $end\n$var wire 8 \" d [7:0] $end\n$var reg 2 $ mem[3] $end\n$upscope $end\n"
             "$upscope $end\n$enddefinitions $end\n"
             "$dumpvars\n0!\nbx \"\nb0 $\nr1.5 #\n$end\n"
             "#0\n0!\n"                      // repeats $dumpvars: not a change
             "#5\n1!\nb101 \"\nb11 $\n"
             "#5\nb110 \"\n"                 // again at 5: the later value stands
             "#10\n0!\n$comment #99 b1 ! $end\n"
             "#15\n1!\nbZ1 $\n";
        for (int t = 20; t < 200000; t += 5) f << "#" << t << "\n" << (t / 5 % 2) << "!\n";
    }
    std::string ref;
    for (unsigned threads : {1u, 2u, 7u}) {
        wave::Reader r;
        r.minPiece = 4096;   // small pieces, so the file really is cut
        std::string err;
        assert(r.open(path, threads, err));
        assert(r.timescale == "10ps" && r.unit > 0.99e-11 && r.unit < 1.01e-11);
        assert(r.end == 199995);
        assert(r.byName.size() == 4 && !r.find("tb.r"));
        assert(r.range["tb.q"] == "[7:0]" && !r.range.count("tb.clk"));
        const wave::Wave* q = r.find("tb.q");
        assert(q && q == r.find("tb.u.d") && q->width == 8);        // an alias
        assert(q->ch.size() == 2 && q->bits(0) == "xxxxxxxx" && q->ch[1].t == 5 && q->bits(1) == "00000110");
        assert(q->at(4) == 0 && q->at(5) == 1 && wave::showBits(q->bits(1)) == "8'h06");
        const wave::Wave* m = r.find("tb.u.mem[3]");
        assert(m && m->ch.size() == 3 && m->bits(2) == "z1");
        const wave::Wave* clk = r.find("tb.clk");
        assert(clk->ch[0].t == 0 && clk->bits(0) == "0" && clk->ch[1].t == 5 && clk->ch[2].t == 10);
        std::string all;
        for (size_t i = 0; i < clk->ch.size(); i++) all += std::to_string(clk->ch[i].t) + clk->bits((int)i);
        if (ref.empty()) ref = all;
        assert(all == ref && clk->ch.size() == 40000);
    }
    wave::Reader bad;
    std::string err;
    assert(!bad.open("no_such_file.vcd", 2, err) && !err.empty());
    std::remove(path);
    return 0;
}
