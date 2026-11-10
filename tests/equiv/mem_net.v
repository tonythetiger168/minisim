// mem.sv synthesized by Yosys (synth -flatten; abc -g AND,NAND,OR,NOR,XOR,XNOR,MUX).

module mem(clk, rst, we, wa, ra, wd, g, li, rd, lo, cnt, prod, bs, ov);
  wire _0000_;
  wire _0001_;
  wire _0002_;
  wire _0003_;
  wire _0004_;
  wire _0005_;
  wire _0006_;
  wire _0007_;
  wire _0008_;
  wire _0009_;
  wire _0010_;
  wire _0011_;
  wire _0012_;
  wire _0013_;
  wire _0014_;
  wire _0015_;
  wire _0016_;
  wire _0017_;
  wire _0018_;
  wire _0019_;
  wire _0020_;
  wire _0021_;
  wire _0022_;
  wire _0023_;
  wire _0024_;
  wire _0025_;
  wire _0026_;
  wire _0027_;
  wire _0028_;
  wire _0029_;
  wire _0030_;
  wire _0031_;
  wire _0032_;
  wire _0033_;
  wire _0034_;
  wire _0035_;
  wire _0036_;
  wire _0037_;
  wire _0038_;
  wire _0039_;
  wire _0040_;
  wire _0041_;
  wire _0042_;
  wire _0043_;
  wire _0044_;
  wire _0045_;
  wire _0046_;
  wire _0047_;
  wire _0048_;
  wire _0049_;
  wire _0050_;
  wire _0051_;
  wire _0052_;
  wire _0053_;
  wire _0054_;
  wire _0055_;
  wire _0056_;
  wire _0057_;
  wire _0058_;
  wire _0059_;
  wire _0060_;
  wire _0061_;
  wire _0062_;
  wire _0063_;
  wire _0064_;
  wire _0065_;
  wire _0066_;
  wire _0067_;
  wire _0068_;
  wire _0069_;
  wire _0070_;
  wire _0071_;
  wire _0072_;
  wire _0073_;
  wire _0074_;
  wire _0075_;
  wire _0076_;
  wire _0077_;
  wire _0078_;
  wire _0079_;
  wire _0080_;
  wire _0081_;
  wire _0082_;
  wire _0083_;
  wire _0084_;
  wire _0085_;
  wire _0086_;
  wire _0087_;
  wire _0088_;
  wire _0089_;
  wire _0090_;
  wire _0091_;
  wire _0092_;
  wire _0093_;
  wire _0094_;
  wire _0095_;
  wire _0096_;
  wire _0097_;
  wire _0098_;
  wire _0099_;
  wire _0100_;
  wire _0101_;
  wire _0102_;
  wire _0103_;
  wire _0104_;
  wire _0105_;
  wire _0106_;
  wire _0107_;
  wire _0108_;
  wire _0109_;
  wire _0110_;
  wire _0111_;
  wire _0112_;
  wire _0113_;
  wire _0114_;
  wire _0115_;
  wire _0116_;
  wire _0117_;
  wire _0118_;
  wire _0119_;
  wire _0120_;
  wire _0121_;
  wire _0122_;
  wire _0123_;
  wire _0124_;
  wire _0125_;
  wire _0126_;
  wire _0127_;
  wire _0128_;
  wire _0129_;
  wire _0130_;
  wire _0131_;
  wire _0132_;
  wire _0133_;
  wire _0134_;
  wire _0135_;
  wire _0136_;
  wire _0137_;
  wire _0138_;
  wire _0139_;
  wire _0140_;
  wire _0141_;
  wire _0142_;
  wire _0143_;
  wire _0144_;
  wire _0145_;
  wire _0146_;
  wire _0147_;
  wire _0148_;
  wire _0149_;
  wire _0150_;
  wire _0151_;
  wire _0152_;
  wire _0153_;
  wire _0154_;
  wire _0155_;
  wire _0156_;
  wire _0157_;
  wire _0158_;
  wire _0159_;
  wire _0160_;
  wire _0161_;
  wire _0162_;
  wire _0163_;
  wire _0164_;
  wire _0165_;
  wire _0166_;
  wire _0167_;
  wire _0168_;
  wire _0169_;
  wire _0170_;
  wire _0171_;
  wire _0172_;
  wire _0173_;
  wire _0174_;
  wire _0175_;
  wire _0176_;
  wire _0177_;
  wire _0178_;
  wire _0179_;
  wire _0180_;
  wire _0181_;
  wire _0182_;
  wire _0183_;
  wire _0184_;
  wire _0185_;
  wire _0186_;
  wire _0187_;
  wire _0188_;
  wire _0189_;
  wire _0190_;
  wire _0191_;
  wire _0192_;
  wire _0193_;
  wire _0194_;
  wire _0195_;
  wire _0196_;
  wire _0197_;
  wire _0198_;
  wire _0199_;
  wire _0200_;
  wire _0201_;
  wire _0202_;
  wire _0203_;
  wire _0204_;
  wire _0205_;
  wire _0206_;
  wire _0207_;
  wire _0208_;
  wire _0209_;
  wire _0210_;
  wire _0211_;
  wire _0212_;
  wire _0213_;
  wire _0214_;
  wire _0215_;
  wire _0216_;
  wire _0217_;
  wire _0218_;
  wire _0219_;
  wire _0220_;
  wire _0221_;
  wire _0222_;
  wire _0223_;
  wire _0224_;
  wire _0225_;
  wire _0226_;
  wire _0227_;
  wire _0228_;
  wire _0229_;
  wire _0230_;
  wire _0231_;
  wire _0232_;
  wire _0233_;
  wire _0234_;
  wire _0235_;
  wire _0236_;
  wire _0237_;
  wire _0238_;
  wire _0239_;
  wire _0240_;
  wire _0241_;
  wire _0242_;
  wire _0243_;
  wire _0244_;
  wire _0245_;
  wire _0246_;
  wire _0247_;
  wire _0248_;
  wire _0249_;
  wire _0250_;
  wire _0251_;
  wire _0252_;
  wire _0253_;
  wire _0254_;
  wire _0255_;
  wire _0256_;
  wire _0257_;
  wire _0258_;
  wire _0259_;
  wire _0260_;
  wire _0261_;
  wire _0262_;
  wire _0263_;
  wire _0264_;
  wire _0265_;
  wire _0266_;
  wire _0267_;
  wire _0268_;
  wire _0269_;
  wire _0270_;
  wire _0271_;
  wire _0272_;
  wire _0273_;
  wire _0274_;
  wire _0275_;
  wire _0276_;
  wire _0277_;
  wire _0278_;
  wire _0279_;
  wire _0280_;
  wire _0281_;
  wire _0282_;
  wire _0283_;
  wire _0284_;
  wire _0285_;
  wire _0286_;
  wire _0287_;
  wire _0288_;
  wire _0289_;
  wire _0290_;
  wire _0291_;
  wire _0292_;
  wire _0293_;
  wire _0294_;
  wire _0295_;
  wire _0296_;
  wire _0297_;
  wire _0298_;
  wire _0299_;
  wire _0300_;
  wire _0301_;
  wire _0302_;
  wire _0303_;
  wire _0304_;
  wire _0305_;
  wire _0306_;
  wire _0307_;
  wire _0308_;
  wire _0309_;
  wire _0310_;
  wire _0311_;
  wire _0312_;
  wire _0313_;
  wire _0314_;
  wire _0315_;
  wire _0316_;
  wire _0317_;
  wire _0318_;
  wire _0319_;
  wire _0320_;
  wire _0321_;
  wire _0322_;
  wire _0323_;
  wire _0324_;
  wire _0325_;
  wire _0326_;
  wire _0327_;
  wire _0328_;
  wire _0329_;
  wire _0330_;
  wire _0331_;
  wire _0332_;
  wire _0333_;
  wire _0334_;
  wire _0335_;
  wire _0336_;
  wire _0337_;
  wire _0338_;
  wire _0339_;
  wire _0340_;
  wire _0341_;
  wire _0342_;
  wire _0343_;
  wire _0344_;
  wire _0345_;
  wire _0346_;
  wire _0347_;
  wire _0348_;
  wire _0349_;
  wire _0350_;
  wire _0351_;
  wire _0352_;
  wire _0353_;
  wire _0354_;
  wire _0355_;
  wire _0356_;
  wire _0357_;
  wire _0358_;
  wire _0359_;
  wire _0360_;
  wire _0361_;
  wire _0362_;
  wire _0363_;
  wire _0364_;
  wire _0365_;
  wire _0366_;
  wire _0367_;
  wire _0368_;
  wire _0369_;
  wire _0370_;
  wire _0371_;
  wire _0372_;
  wire _0373_;
  wire _0374_;
  wire _0375_;
  wire _0376_;
  wire _0377_;
  wire _0378_;
  wire _0379_;
  wire _0380_;
  wire _0381_;
  wire _0382_;
  wire _0383_;
  wire _0384_;
  wire _0385_;
  wire _0386_;
  wire _0387_;
  wire _0388_;
  wire _0389_;
  wire _0390_;
  wire _0391_;
  wire _0392_;
  wire _0393_;
  wire _0394_;
  wire _0395_;
  wire _0396_;
  wire _0397_;
  wire _0398_;
  wire _0399_;
  wire _0400_;
  wire _0401_;
  wire _0402_;
  wire _0403_;
  wire _0404_;
  wire _0405_;
  wire _0406_;
  wire _0407_;
  wire _0408_;
  wire _0409_;
  wire _0410_;
  wire _0411_;
  wire _0412_;
  wire _0413_;
  wire _0414_;
  wire _0415_;
  wire _0416_;
  wire _0417_;
  wire _0418_;
  wire _0419_;
  wire _0420_;
  wire _0421_;
  wire _0422_;
  wire _0423_;
  wire _0424_;
  wire _0425_;
  wire _0426_;
  wire _0427_;
  wire _0428_;
  wire _0429_;
  wire _0430_;
  wire _0431_;
  wire _0432_;
  wire _0433_;
  wire _0434_;
  wire _0435_;
  wire _0436_;
  wire _0437_;
  wire _0438_;
  wire _0439_;
  wire _0440_;
  wire _0441_;
  wire _0442_;
  wire _0443_;
  wire _0444_;
  wire _0445_;
  wire _0446_;
  wire _0447_;
  wire _0448_;
  wire _0449_;
  wire _0450_;
  wire _0451_;
  wire _0452_;
  wire _0453_;
  wire _0454_;
  wire _0455_;
  wire _0456_;
  wire _0457_;
  wire _0458_;
  wire _0459_;
  wire _0460_;
  wire _0461_;
  wire _0462_;
  wire _0463_;
  wire _0464_;
  wire _0465_;
  wire _0466_;
  wire _0467_;
  wire _0468_;
  wire _0469_;
  wire _0470_;
  wire _0471_;
  wire _0472_;
  wire _0473_;
  wire _0474_;
  wire _0475_;
  wire _0476_;
  wire _0477_;
  wire _0478_;
  wire _0479_;
  wire _0480_;
  wire _0481_;
  wire _0482_;
  wire _0483_;
  wire _0484_;
  wire _0485_;
  wire _0486_;
  wire _0487_;
  wire _0488_;
  wire _0489_;
  wire _0490_;
  wire _0491_;
  wire _0492_;
  wire _0493_;
  wire _0494_;
  wire _0495_;
  wire _0496_;
  wire _0497_;
  wire _0498_;
  wire _0499_;
  wire _0500_;
  wire _0501_;
  wire _0502_;
  wire _0503_;
  wire _0504_;
  wire _0505_;
  wire _0506_;
  wire _0507_;
  wire _0508_;
  wire _0509_;
  wire _0510_;
  wire _0511_;
  wire _0512_;
  wire _0513_;
  wire _0514_;
  wire _0515_;
  wire _0516_;
  wire _0517_;
  wire _0518_;
  wire _0519_;
  wire _0520_;
  wire _0521_;
  wire _0522_;
  wire _0523_;
  wire _0524_;
  wire _0525_;
  wire [7:0] _0526_;
  wire [7:0] _0527_;
  output [7:0] bs;
  wire [7:0] bs;
  input clk;
  wire clk;
  output [7:0] cnt;
  reg [7:0] cnt;
  input g;
  wire g;
  input [7:0] li;
  wire [7:0] li;
  output [7:0] lo;
  reg [7:0] lo;
  reg [7:0] \mem[0] ;
  reg [7:0] \mem[1] ;
  reg [7:0] \mem[2] ;
  reg [7:0] \mem[3] ;
  reg [7:0] \mem[4] ;
  reg [7:0] \mem[5] ;
  reg [7:0] \mem[6] ;
  reg [7:0] \mem[7] ;
  output ov;
  wire ov;
  output [15:0] prod;
  wire [15:0] prod;
  input [2:0] ra;
  wire [2:0] ra;
  output [7:0] rd;
  wire [7:0] rd;
  input rst;
  wire rst;
  input [2:0] wa;
  wire [2:0] wa;
  input [7:0] wd;
  wire [7:0] wd;
  input we;
  wire we;
  assign _0016_ = ~wa[0];
  assign _0017_ = ~wa[2];
  assign _0018_ = ~rst;
  assign _0019_ = ~ra[0];
  assign _0020_ = ~wd[1];
  assign _0021_ = ~wd[0];
  assign _0022_ = ~wd[2];
  assign _0023_ = ~wd[3];
  assign _0024_ = ~wd[4];
  assign _0025_ = ~wd[6];
  assign _0026_ = ~wd[5];
  assign _0027_ = ~(wa[1] | wa[2]);
  assign _0028_ = we & _0018_;
  assign _0029_ = wa[0] & _0028_;
  assign _0009_ = _0027_ & _0029_;
  assign _0006_ = rst | _0009_;
  assign _0030_ = _0016_ & _0028_;
  assign _0008_ = _0027_ & _0030_;
  assign _0007_ = rst | _0008_;
  assign _0031_ = wa[1] & _0017_;
  assign _0011_ = _0029_ & _0031_;
  assign _0004_ = rst | _0011_;
  assign _0010_ = _0030_ & _0031_;
  assign _0005_ = rst | _0010_;
  assign _0032_ = ~(wa[1] | _0017_);
  assign _0012_ = _0030_ & _0032_;
  assign _0003_ = rst | _0012_;
  assign _0013_ = _0029_ & _0032_;
  assign _0002_ = rst | _0013_;
  assign _0033_ = wa[1] & wa[2];
  assign _0014_ = _0030_ & _0033_;
  assign _0001_ = rst | _0014_;
  assign _0015_ = _0029_ & _0033_;
  assign _0000_ = rst | _0015_;
  assign _0034_ = ra[0] ? \mem[7] [0] : \mem[6] [0];
  assign _0035_ = ra[0] ? \mem[5] [0] : \mem[4] [0];
  assign _0036_ = ra[1] ? _0034_ : _0035_;
  assign _0037_ = ra[0] ? \mem[3] [0] : \mem[2] [0];
  assign _0038_ = ra[0] ? \mem[1] [0] : \mem[0] [0];
  assign _0039_ = ra[1] ? _0037_ : _0038_;
  assign rd[0] = ra[2] ? _0036_ : _0039_;
  assign _0040_ = ra[0] ? \mem[7] [1] : \mem[6] [1];
  assign _0041_ = ra[0] ? \mem[5] [1] : \mem[4] [1];
  assign _0042_ = ra[1] ? _0040_ : _0041_;
  assign _0043_ = ra[0] ? \mem[3] [1] : \mem[2] [1];
  assign _0044_ = ra[0] ? \mem[1] [1] : \mem[0] [1];
  assign _0045_ = ra[1] ? _0043_ : _0044_;
  assign rd[1] = ra[2] ? _0042_ : _0045_;
  assign _0046_ = ra[0] ? \mem[7] [2] : \mem[6] [2];
  assign _0047_ = ra[0] ? \mem[5] [2] : \mem[4] [2];
  assign _0048_ = ra[1] ? _0046_ : _0047_;
  assign _0049_ = ra[0] ? \mem[3] [2] : \mem[2] [2];
  assign _0050_ = ra[0] ? \mem[1] [2] : \mem[0] [2];
  assign _0051_ = ra[1] ? _0049_ : _0050_;
  assign rd[2] = ra[2] ? _0048_ : _0051_;
  assign _0052_ = ra[0] ? \mem[7] [3] : \mem[6] [3];
  assign _0053_ = ra[0] ? \mem[5] [3] : \mem[4] [3];
  assign _0054_ = ra[1] ? _0052_ : _0053_;
  assign _0055_ = ra[0] ? \mem[3] [3] : \mem[2] [3];
  assign _0056_ = ra[0] ? \mem[1] [3] : \mem[0] [3];
  assign _0057_ = ra[1] ? _0055_ : _0056_;
  assign rd[3] = ra[2] ? _0054_ : _0057_;
  assign _0058_ = ra[0] ? \mem[7] [4] : \mem[6] [4];
  assign _0059_ = ra[0] ? \mem[5] [4] : \mem[4] [4];
  assign _0060_ = ra[1] ? _0058_ : _0059_;
  assign _0061_ = ra[0] ? \mem[3] [4] : \mem[2] [4];
  assign _0062_ = ra[0] ? \mem[1] [4] : \mem[0] [4];
  assign _0063_ = ra[1] ? _0061_ : _0062_;
  assign rd[4] = ra[2] ? _0060_ : _0063_;
  assign _0064_ = ra[0] ? \mem[7] [5] : \mem[6] [5];
  assign _0065_ = ra[0] ? \mem[5] [5] : \mem[4] [5];
  assign _0066_ = ra[1] ? _0064_ : _0065_;
  assign _0067_ = ra[0] ? \mem[3] [5] : \mem[2] [5];
  assign _0068_ = ra[0] ? \mem[1] [5] : \mem[0] [5];
  assign _0069_ = ra[1] ? _0067_ : _0068_;
  assign rd[5] = ra[2] ? _0066_ : _0069_;
  assign _0070_ = ra[0] ? \mem[7] [6] : \mem[6] [6];
  assign _0071_ = ra[0] ? \mem[5] [6] : \mem[4] [6];
  assign _0072_ = ra[1] ? _0070_ : _0071_;
  assign _0073_ = ra[0] ? \mem[3] [6] : \mem[2] [6];
  assign _0074_ = ra[0] ? \mem[1] [6] : \mem[0] [6];
  assign _0075_ = ra[1] ? _0073_ : _0074_;
  assign rd[6] = ra[2] ? _0072_ : _0075_;
  assign _0076_ = ra[0] ? \mem[7] [7] : \mem[6] [7];
  assign _0077_ = ra[0] ? \mem[5] [7] : \mem[4] [7];
  assign _0078_ = ra[1] ? _0076_ : _0077_;
  assign _0079_ = ra[0] ? \mem[3] [7] : \mem[2] [7];
  assign _0080_ = ra[0] ? \mem[1] [7] : \mem[0] [7];
  assign _0081_ = ra[1] ? _0079_ : _0080_;
  assign rd[7] = ra[2] ? _0078_ : _0081_;
  assign _0082_ = li[1] & wd[0];
  assign _0083_ = li[0] & wd[1];
  assign _0084_ = _0082_ & _0083_;
  assign prod[1] = _0082_ ^ _0083_;
  assign _0085_ = wd[0] & li[2];
  assign _0086_ = wd[1] & li[1];
  assign _0087_ = li[0] & wd[2];
  assign _0088_ = ~(_0086_ & _0087_);
  assign _0089_ = _0086_ ^ _0087_;
  assign _0090_ = ~(_0085_ & _0089_);
  assign _0091_ = _0085_ ^ _0089_;
  assign _0092_ = _0084_ & _0091_;
  assign prod[2] = _0084_ ^ _0091_;
  assign _0093_ = wd[0] & li[3];
  assign _0094_ = ~(_0088_ & _0090_);
  assign _0095_ = wd[1] & li[2];
  assign _0096_ = li[1] & wd[2];
  assign _0097_ = li[0] & wd[3];
  assign _0098_ = ~(_0096_ & _0097_);
  assign _0099_ = _0096_ ^ _0097_;
  assign _0100_ = ~(_0095_ & _0099_);
  assign _0101_ = _0095_ ^ _0099_;
  assign _0102_ = ~(_0094_ & _0101_);
  assign _0103_ = _0094_ ^ _0101_;
  assign _0104_ = ~(_0093_ & _0103_);
  assign _0105_ = _0093_ ^ _0103_;
  assign _0106_ = _0092_ & _0105_;
  assign prod[3] = _0092_ ^ _0105_;
  assign _0107_ = ~(_0102_ & _0104_);
  assign _0108_ = wd[0] & li[4];
  assign _0109_ = wd[1] & li[3];
  assign _0110_ = _0108_ & _0109_;
  assign _0111_ = _0108_ ^ _0109_;
  assign _0112_ = ~(_0098_ & _0100_);
  assign _0113_ = wd[2] & li[2];
  assign _0114_ = li[1] & wd[3];
  assign _0115_ = li[0] & wd[4];
  assign _0116_ = ~(_0114_ & _0115_);
  assign _0117_ = _0114_ ^ _0115_;
  assign _0118_ = ~(_0113_ & _0117_);
  assign _0119_ = _0113_ ^ _0117_;
  assign _0120_ = ~(_0112_ & _0119_);
  assign _0121_ = _0112_ ^ _0119_;
  assign _0122_ = ~(_0111_ & _0121_);
  assign _0123_ = _0111_ ^ _0121_;
  assign _0124_ = _0107_ & _0123_;
  assign _0125_ = _0107_ ^ _0123_;
  assign _0126_ = _0106_ & _0125_;
  assign prod[4] = _0106_ ^ _0125_;
  assign _0127_ = ~(_0120_ & _0122_);
  assign _0128_ = ~(_0116_ & _0118_);
  assign _0129_ = li[1] & wd[4];
  assign _0130_ = li[0] & wd[5];
  assign _0131_ = ~(_0129_ & _0130_);
  assign _0132_ = _0129_ ^ _0130_;
  assign _0133_ = li[2] & wd[3];
  assign _0134_ = ~(_0132_ & _0133_);
  assign _0135_ = _0132_ ^ _0133_;
  assign _0136_ = ~(_0128_ & _0135_);
  assign _0137_ = _0128_ ^ _0135_;
  assign _0138_ = wd[0] & li[5];
  assign _0139_ = wd[1] & li[4];
  assign _0140_ = wd[2] & li[3];
  assign _0141_ = ~(_0139_ & _0140_);
  assign _0142_ = _0139_ ^ _0140_;
  assign _0143_ = ~(_0138_ & _0142_);
  assign _0144_ = _0138_ ^ _0142_;
  assign _0145_ = ~(_0137_ & _0144_);
  assign _0146_ = _0137_ ^ _0144_;
  assign _0147_ = ~(_0127_ & _0146_);
  assign _0148_ = _0127_ ^ _0146_;
  assign _0149_ = ~(_0110_ & _0148_);
  assign _0150_ = _0110_ ^ _0148_;
  assign _0151_ = _0124_ & _0150_;
  assign _0152_ = _0124_ ^ _0150_;
  assign _0153_ = _0126_ & _0152_;
  assign _0154_ = ~(_0147_ & _0149_);
  assign _0155_ = wd[0] & li[6];
  assign _0156_ = ~(_0141_ & _0143_);
  assign _0157_ = _0155_ & _0156_;
  assign _0158_ = _0155_ ^ _0156_;
  assign _0159_ = ~(_0136_ & _0145_);
  assign _0160_ = wd[1] & li[5];
  assign _0161_ = wd[2] & li[4];
  assign _0162_ = wd[3] & li[3];
  assign _0163_ = ~(_0161_ & _0162_);
  assign _0164_ = _0161_ ^ _0162_;
  assign _0165_ = ~(_0160_ & _0164_);
  assign _0166_ = _0160_ ^ _0164_;
  assign _0167_ = ~(_0131_ & _0134_);
  assign _0168_ = li[2] & wd[4];
  assign _0169_ = li[1] & wd[5];
  assign _0170_ = li[0] & wd[6];
  assign _0171_ = ~(_0169_ & _0170_);
  assign _0172_ = _0169_ ^ _0170_;
  assign _0173_ = ~(_0168_ & _0172_);
  assign _0174_ = _0168_ ^ _0172_;
  assign _0175_ = ~(_0167_ & _0174_);
  assign _0176_ = _0167_ ^ _0174_;
  assign _0177_ = ~(_0166_ & _0176_);
  assign _0178_ = _0166_ ^ _0176_;
  assign _0179_ = ~(_0159_ & _0178_);
  assign _0180_ = _0159_ ^ _0178_;
  assign _0181_ = ~(_0158_ & _0180_);
  assign _0182_ = _0158_ ^ _0180_;
  assign _0183_ = _0154_ & _0182_;
  assign _0184_ = _0154_ ^ _0182_;
  assign _0185_ = ~(_0151_ & _0184_);
  assign _0186_ = _0151_ ^ _0184_;
  assign _0187_ = ~(_0153_ & _0186_);
  assign prod[6] = _0153_ ^ _0186_;
  assign _0188_ = ~(_0185_ & _0187_);
  assign _0189_ = ~(_0179_ & _0181_);
  assign _0190_ = _0021_ & li[7];
  assign _0191_ = wd[1] & li[6];
  assign _0192_ = wd[2] & li[5];
  assign _0193_ = ~(_0191_ & _0192_);
  assign _0194_ = _0191_ ^ _0192_;
  assign _0195_ = ~(_0190_ & _0194_);
  assign _0196_ = _0190_ ^ _0194_;
  assign _0197_ = ~(_0163_ & _0165_);
  assign _0198_ = _0196_ & _0197_;
  assign _0199_ = _0196_ ^ _0197_;
  assign _0200_ = ~(_0175_ & _0177_);
  assign _0201_ = wd[3] & li[4];
  assign _0202_ = li[3] & wd[4];
  assign _0203_ = li[2] & wd[5];
  assign _0204_ = ~(_0202_ & _0203_);
  assign _0205_ = _0202_ ^ _0203_;
  assign _0206_ = ~(_0201_ & _0205_);
  assign _0207_ = _0201_ ^ _0205_;
  assign _0208_ = ~(_0171_ & _0173_);
  assign _0209_ = ~(li[1] & wd[6]);
  assign _0210_ = li[0] & wd[7];
  assign _0211_ = ~(li[7] & _0210_);
  assign _0212_ = ~(li[7] ^ _0210_);
  assign _0213_ = _0209_ | _0212_;
  assign _0214_ = _0209_ ^ _0212_;
  assign _0215_ = ~(_0208_ & _0214_);
  assign _0216_ = _0208_ ^ _0214_;
  assign _0217_ = ~(_0207_ & _0216_);
  assign _0218_ = _0207_ ^ _0216_;
  assign _0219_ = ~(_0200_ & _0218_);
  assign _0220_ = _0200_ ^ _0218_;
  assign _0221_ = ~(_0199_ & _0220_);
  assign _0222_ = _0199_ ^ _0220_;
  assign _0223_ = ~(_0189_ & _0222_);
  assign _0224_ = _0189_ ^ _0222_;
  assign _0225_ = ~(_0157_ & _0224_);
  assign _0226_ = _0157_ ^ _0224_;
  assign _0227_ = ~(_0183_ & _0226_);
  assign _0228_ = _0183_ ^ _0226_;
  assign _0229_ = ~(_0188_ & _0228_);
  assign prod[7] = _0188_ ^ _0228_;
  assign _0230_ = ~(_0227_ & _0229_);
  assign _0231_ = ~(_0223_ & _0225_);
  assign _0232_ = ~(_0219_ & _0221_);
  assign _0233_ = ~(_0193_ & _0195_);
  assign _0234_ = _0020_ & li[7];
  assign _0235_ = wd[2] & li[6];
  assign _0236_ = _0234_ & _0235_;
  assign _0237_ = _0234_ ^ _0235_;
  assign _0238_ = ~(_0204_ & _0206_);
  assign _0239_ = ~(_0237_ & _0238_);
  assign _0240_ = _0237_ ^ _0238_;
  assign _0241_ = ~(_0233_ & _0240_);
  assign _0242_ = _0233_ ^ _0240_;
  assign _0243_ = ~(_0215_ & _0217_);
  assign _0244_ = wd[3] & li[5];
  assign _0245_ = wd[4] & li[4];
  assign _0246_ = li[3] & wd[5];
  assign _0247_ = ~(_0245_ & _0246_);
  assign _0248_ = _0245_ ^ _0246_;
  assign _0249_ = ~(_0244_ & _0248_);
  assign _0250_ = _0244_ ^ _0248_;
  assign _0251_ = ~(_0211_ & _0213_);
  assign _0252_ = li[2] & wd[6];
  assign _0253_ = li[1] & wd[7];
  assign _0254_ = ~(_0210_ & _0253_);
  assign _0255_ = _0210_ ^ _0253_;
  assign _0256_ = ~(_0252_ & _0255_);
  assign _0257_ = _0252_ ^ _0255_;
  assign _0258_ = ~(_0251_ & _0257_);
  assign _0259_ = _0251_ ^ _0257_;
  assign _0260_ = ~(_0250_ & _0259_);
  assign _0261_ = _0250_ ^ _0259_;
  assign _0262_ = ~(_0243_ & _0261_);
  assign _0263_ = _0243_ ^ _0261_;
  assign _0264_ = ~(_0242_ & _0263_);
  assign _0265_ = _0242_ ^ _0263_;
  assign _0266_ = ~(_0232_ & _0265_);
  assign _0267_ = _0232_ ^ _0265_;
  assign _0268_ = ~(_0198_ & _0267_);
  assign _0269_ = _0198_ ^ _0267_;
  assign _0270_ = ~(_0231_ & _0269_);
  assign _0271_ = _0231_ ^ _0269_;
  assign _0272_ = ~(_0230_ & _0271_);
  assign prod[8] = _0230_ ^ _0271_;
  assign _0273_ = ~(_0270_ & _0272_);
  assign _0274_ = ~(_0266_ & _0268_);
  assign _0275_ = ~(_0239_ & _0241_);
  assign _0276_ = ~(_0262_ & _0264_);
  assign _0277_ = _0022_ & li[7];
  assign _0278_ = wd[3] & li[6];
  assign _0279_ = _0277_ & _0278_;
  assign _0280_ = _0277_ ^ _0278_;
  assign _0281_ = ~(_0247_ & _0249_);
  assign _0282_ = ~(_0280_ & _0281_);
  assign _0283_ = _0280_ ^ _0281_;
  assign _0284_ = ~(_0236_ & _0283_);
  assign _0285_ = _0236_ ^ _0283_;
  assign _0286_ = ~(_0258_ & _0260_);
  assign _0287_ = wd[4] & li[5];
  assign _0288_ = li[4] & wd[5];
  assign _0289_ = li[3] & wd[6];
  assign _0290_ = ~(_0288_ & _0289_);
  assign _0291_ = _0288_ ^ _0289_;
  assign _0292_ = ~(_0287_ & _0291_);
  assign _0293_ = _0287_ ^ _0291_;
  assign _0294_ = ~(_0254_ & _0256_);
  assign _0295_ = ~_0294_;
  assign _0296_ = li[2] & wd[7];
  assign _0297_ = ~(_0255_ & _0296_);
  assign _0298_ = _0255_ ^ _0296_;
  assign _0299_ = _0294_ ^ _0298_;
  assign _0300_ = ~(_0293_ & _0299_);
  assign _0301_ = _0293_ ^ _0299_;
  assign _0302_ = ~(_0286_ & _0301_);
  assign _0303_ = _0286_ ^ _0301_;
  assign _0304_ = ~(_0285_ & _0303_);
  assign _0305_ = _0285_ ^ _0303_;
  assign _0306_ = ~(_0276_ & _0305_);
  assign _0307_ = _0276_ ^ _0305_;
  assign _0308_ = ~(_0275_ & _0307_);
  assign _0309_ = _0275_ ^ _0307_;
  assign _0310_ = ~(_0274_ & _0309_);
  assign _0311_ = _0274_ ^ _0309_;
  assign _0312_ = ~(_0273_ & _0311_);
  assign prod[9] = _0273_ ^ _0311_;
  assign _0313_ = ~(_0310_ & _0312_);
  assign _0314_ = ~(_0306_ & _0308_);
  assign _0315_ = ~(_0282_ & _0284_);
  assign _0316_ = ~(_0302_ & _0304_);
  assign _0317_ = _0023_ & li[7];
  assign _0318_ = wd[4] & li[6];
  assign _0319_ = _0317_ & _0318_;
  assign _0320_ = _0317_ ^ _0318_;
  assign _0321_ = ~(_0290_ & _0292_);
  assign _0322_ = ~(_0320_ & _0321_);
  assign _0323_ = _0320_ ^ _0321_;
  assign _0324_ = ~(_0279_ & _0323_);
  assign _0325_ = _0279_ ^ _0323_;
  assign _0326_ = ~(_0294_ & _0298_);
  assign _0327_ = ~(_0300_ & _0326_);
  assign _0328_ = wd[5] & li[5];
  assign _0329_ = li[4] & wd[6];
  assign _0330_ = li[3] & wd[7];
  assign _0331_ = ~(_0329_ & _0330_);
  assign _0332_ = _0329_ ^ _0330_;
  assign _0333_ = ~(_0328_ & _0332_);
  assign _0334_ = _0328_ ^ _0332_;
  assign _0335_ = ~(_0254_ & _0297_);
  assign _0336_ = _0298_ ? _0295_ : _0335_;
  assign _0337_ = ~(_0334_ & _0336_);
  assign _0338_ = _0334_ ^ _0336_;
  assign _0339_ = ~(_0327_ & _0338_);
  assign _0340_ = _0327_ ^ _0338_;
  assign _0341_ = ~(_0325_ & _0340_);
  assign _0342_ = _0325_ ^ _0340_;
  assign _0343_ = ~(_0316_ & _0342_);
  assign _0344_ = _0316_ ^ _0342_;
  assign _0345_ = ~(_0315_ & _0344_);
  assign _0346_ = _0315_ ^ _0344_;
  assign _0347_ = ~(_0314_ & _0346_);
  assign _0348_ = _0314_ ^ _0346_;
  assign _0349_ = ~(_0313_ & _0348_);
  assign prod[10] = _0313_ ^ _0348_;
  assign _0350_ = ~(_0347_ & _0349_);
  assign _0351_ = ~(_0343_ & _0345_);
  assign _0352_ = ~(_0322_ & _0324_);
  assign _0353_ = ~(_0339_ & _0341_);
  assign _0354_ = _0024_ & li[7];
  assign _0355_ = wd[5] & li[6];
  assign _0356_ = _0354_ & _0355_;
  assign _0357_ = _0354_ ^ _0355_;
  assign _0358_ = ~(_0331_ & _0333_);
  assign _0359_ = ~(_0357_ & _0358_);
  assign _0360_ = _0357_ ^ _0358_;
  assign _0361_ = ~(_0319_ & _0360_);
  assign _0362_ = _0319_ ^ _0360_;
  assign _0363_ = ~(_0326_ & _0337_);
  assign _0364_ = wd[6] & li[5];
  assign _0365_ = li[4] & wd[7];
  assign _0366_ = ~(_0330_ & _0365_);
  assign _0367_ = _0330_ ^ _0365_;
  assign _0368_ = ~(_0364_ & _0367_);
  assign _0369_ = _0364_ ^ _0367_;
  assign _0370_ = ~(_0336_ & _0369_);
  assign _0371_ = _0336_ ^ _0369_;
  assign _0372_ = ~(_0363_ & _0371_);
  assign _0373_ = _0363_ ^ _0371_;
  assign _0374_ = ~(_0362_ & _0373_);
  assign _0375_ = _0362_ ^ _0373_;
  assign _0376_ = ~(_0353_ & _0375_);
  assign _0377_ = _0353_ ^ _0375_;
  assign _0378_ = ~(_0352_ & _0377_);
  assign _0379_ = _0352_ ^ _0377_;
  assign _0380_ = ~(_0351_ & _0379_);
  assign _0381_ = _0351_ ^ _0379_;
  assign _0382_ = ~(_0350_ & _0381_);
  assign prod[11] = _0350_ ^ _0381_;
  assign _0383_ = ~(_0380_ & _0382_);
  assign _0384_ = ~(_0376_ & _0378_);
  assign _0385_ = ~(_0359_ & _0361_);
  assign _0386_ = ~(_0372_ & _0374_);
  assign _0387_ = _0026_ & li[7];
  assign _0388_ = wd[6] & li[6];
  assign _0389_ = _0387_ & _0388_;
  assign _0390_ = _0387_ ^ _0388_;
  assign _0391_ = ~(_0366_ & _0368_);
  assign _0392_ = ~(_0390_ & _0391_);
  assign _0393_ = _0390_ ^ _0391_;
  assign _0394_ = ~(_0356_ & _0393_);
  assign _0395_ = _0356_ ^ _0393_;
  assign _0396_ = ~(_0326_ & _0370_);
  assign _0397_ = li[5] & wd[7];
  assign _0398_ = ~(_0367_ & _0397_);
  assign _0399_ = _0367_ ^ _0397_;
  assign _0400_ = ~(_0336_ & _0399_);
  assign _0401_ = _0336_ ^ _0399_;
  assign _0402_ = ~(_0396_ & _0401_);
  assign _0403_ = _0396_ ^ _0401_;
  assign _0404_ = ~(_0395_ & _0403_);
  assign _0405_ = _0395_ ^ _0403_;
  assign _0406_ = ~(_0386_ & _0405_);
  assign _0407_ = _0386_ ^ _0405_;
  assign _0408_ = ~(_0385_ & _0407_);
  assign _0409_ = _0385_ ^ _0407_;
  assign _0410_ = ~(_0384_ & _0409_);
  assign _0411_ = _0384_ ^ _0409_;
  assign _0412_ = ~(_0383_ & _0411_);
  assign prod[12] = _0383_ ^ _0411_;
  assign _0413_ = ~(_0410_ & _0412_);
  assign _0414_ = ~(_0406_ & _0408_);
  assign _0415_ = ~(_0392_ & _0394_);
  assign _0416_ = ~(_0402_ & _0404_);
  assign _0417_ = _0025_ & li[7];
  assign _0418_ = li[6] & wd[7];
  assign _0419_ = _0417_ & _0418_;
  assign _0420_ = _0417_ ^ _0418_;
  assign _0421_ = _0366_ & _0398_;
  assign _0422_ = ~(_0366_ & _0398_);
  assign _0423_ = ~(_0420_ & _0422_);
  assign _0424_ = ~(_0420_ ^ _0421_);
  assign _0425_ = ~(_0389_ & _0424_);
  assign _0426_ = _0389_ ^ _0424_;
  assign _0427_ = _0326_ & _0400_;
  assign _0428_ = ~_0427_;
  assign _0429_ = ~(_0401_ & _0428_);
  assign _0430_ = ~(_0401_ ^ _0427_);
  assign _0431_ = ~(_0426_ & _0430_);
  assign _0432_ = _0426_ ^ _0430_;
  assign _0433_ = ~(_0416_ & _0432_);
  assign _0434_ = _0416_ ^ _0432_;
  assign _0435_ = ~(_0415_ & _0434_);
  assign _0436_ = _0415_ ^ _0434_;
  assign _0437_ = ~(_0414_ & _0436_);
  assign _0438_ = _0414_ ^ _0436_;
  assign _0439_ = ~(_0413_ & _0438_);
  assign prod[13] = _0413_ ^ _0438_;
  assign _0440_ = ~(_0437_ & _0439_);
  assign _0441_ = ~(_0433_ & _0435_);
  assign _0442_ = ~(_0423_ & _0425_);
  assign _0443_ = ~(_0429_ & _0431_);
  assign _0444_ = wd[7] ? li[6] : li[7];
  assign _0445_ = ~(_0422_ & _0444_);
  assign _0446_ = ~(_0421_ ^ _0444_);
  assign _0447_ = ~(_0419_ & _0446_);
  assign _0448_ = _0419_ ^ _0446_;
  assign _0449_ = ~(_0430_ & _0448_);
  assign _0450_ = _0430_ ^ _0448_;
  assign _0451_ = ~(_0443_ & _0450_);
  assign _0452_ = _0443_ ^ _0450_;
  assign _0453_ = ~(_0442_ & _0452_);
  assign _0454_ = _0442_ ^ _0452_;
  assign _0455_ = ~(_0441_ & _0454_);
  assign _0456_ = _0441_ ^ _0454_;
  assign _0457_ = ~(_0440_ & _0456_);
  assign prod[14] = _0440_ ^ _0456_;
  assign _0458_ = _0451_ & _0453_;
  assign _0459_ = ~(_0445_ & _0447_);
  assign _0460_ = ~(_0429_ & _0449_);
  assign _0461_ = ~(_0430_ ^ _0446_);
  assign _0462_ = ~(_0460_ ^ _0461_);
  assign _0463_ = ~(_0459_ ^ _0462_);
  assign _0464_ = ~(_0458_ & _0463_);
  assign _0465_ = _0458_ | _0463_;
  assign _0466_ = _0455_ & _0465_;
  assign _0467_ = _0464_ & _0466_;
  assign prod[15] = _0457_ & _0467_;
  assign prod[5] = _0126_ ^ _0152_;
  assign _0468_ = ra[0] ? wd[4] : wd[3];
  assign _0469_ = ra[0] ? wd[2] : wd[1];
  assign _0470_ = ra[0] ? wd[6] : wd[5];
  assign _0471_ = ra[2] ? _0470_ : _0469_;
  assign _0472_ = _0019_ & wd[7];
  assign _0473_ = ra[2] ? _0472_ : _0468_;
  assign _0474_ = ra[1] ? _0473_ : _0471_;
  assign _0475_ = ~(li[5] & _0474_);
  assign _0476_ = ra[0] ? wd[7] : wd[6];
  assign _0477_ = ra[0] ? wd[5] : wd[4];
  assign _0478_ = ra[1] ? _0476_ : _0477_;
  assign _0479_ = ra[0] ? wd[3] : wd[2];
  assign _0480_ = ra[0] ? wd[1] : wd[0];
  assign _0481_ = ra[1] ? _0479_ : _0480_;
  assign _0482_ = ra[2] ? _0478_ : _0481_;
  assign _0483_ = li[4] & _0482_;
  assign _0484_ = li[5] ^ _0474_;
  assign _0485_ = ~(_0483_ & _0484_);
  assign _0486_ = ~(_0475_ & _0485_);
  assign _0487_ = li[6] & _0486_;
  assign bs[4] = li[7] & _0487_;
  assign _0488_ = cnt[1] & cnt[0];
  assign _0489_ = cnt[3] & cnt[2];
  assign _0490_ = ~(_0488_ & _0489_);
  assign _0491_ = ~(wd[4] | wd[6]);
  assign _0492_ = ~(wd[5] | wd[7]);
  assign _0493_ = _0491_ & _0492_;
  assign _0494_ = wd[0] & _0022_;
  assign _0495_ = wd[1] ^ wd[3];
  assign _0496_ = _0494_ & _0495_;
  assign _0497_ = ~(_0493_ & _0496_);
  assign ov = ~(_0490_ & _0497_);
  assign prod[0] = li[0] & wd[0];
  assign bs[1] = _0483_ ^ _0484_;
  assign bs[2] = li[6] ^ _0486_;
  assign bs[3] = li[7] ^ _0487_;
  assign bs[0] = li[4] ^ _0482_;
  assign _0498_ = wd[0] & cnt[0];
  assign _0499_ = ~(wd[1] & cnt[1]);
  assign _0500_ = wd[1] ^ cnt[1];
  assign _0501_ = ~(_0498_ & _0500_);
  assign _0527_[1] = _0498_ ^ _0500_;
  assign _0502_ = ~(_0499_ & _0501_);
  assign _0503_ = ~(wd[2] & cnt[2]);
  assign _0504_ = wd[2] ^ cnt[2];
  assign _0505_ = ~(_0502_ & _0504_);
  assign _0527_[2] = _0502_ ^ _0504_;
  assign _0506_ = _0503_ & _0505_;
  assign _0507_ = ~(wd[3] | cnt[3]);
  assign _0508_ = ~(wd[3] & cnt[3]);
  assign _0509_ = wd[3] ^ cnt[3];
  assign _0527_[3] = ~(_0506_ ^ _0509_);
  assign _0510_ = _0506_ | _0507_;
  assign _0511_ = ~(_0508_ & _0510_);
  assign _0512_ = ~(wd[4] & cnt[4]);
  assign _0513_ = wd[4] ^ cnt[4];
  assign _0514_ = ~(_0511_ & _0513_);
  assign _0527_[4] = _0511_ ^ _0513_;
  assign _0515_ = _0512_ & _0514_;
  assign _0516_ = ~(wd[5] | cnt[5]);
  assign _0517_ = ~(wd[5] & cnt[5]);
  assign _0518_ = wd[5] ^ cnt[5];
  assign _0527_[5] = ~(_0515_ ^ _0518_);
  assign _0519_ = _0515_ | _0516_;
  assign _0520_ = ~(_0517_ & _0519_);
  assign _0521_ = ~(wd[6] & cnt[6]);
  assign _0522_ = wd[6] ^ cnt[6];
  assign _0523_ = ~(_0520_ & _0522_);
  assign _0527_[6] = _0520_ ^ _0522_;
  assign _0524_ = _0521_ & _0523_;
  assign _0525_ = wd[7] ^ cnt[7];
  assign _0527_[7] = ~(_0524_ ^ _0525_);
  assign _0526_[0] = wd[0] ^ cnt[0];
  always @(posedge clk, posedge rst)
    if (rst) cnt[0] <= 1'h1;
    else if (we) cnt[0] <= _0526_[0];
  always @(posedge clk, posedge rst)
    if (rst) cnt[1] <= 1'h1;
    else if (we) cnt[1] <= _0527_[1];
  always @(posedge clk, posedge rst)
    if (rst) cnt[2] <= 1'h1;
    else if (we) cnt[2] <= _0527_[2];
  always @(posedge clk, posedge rst)
    if (rst) cnt[3] <= 1'h1;
    else if (we) cnt[3] <= _0527_[3];
  always @(posedge clk, posedge rst)
    if (rst) cnt[4] <= 1'h1;
    else if (we) cnt[4] <= _0527_[4];
  always @(posedge clk, posedge rst)
    if (rst) cnt[5] <= 1'h1;
    else if (we) cnt[5] <= _0527_[5];
  always @(posedge clk, posedge rst)
    if (rst) cnt[6] <= 1'h1;
    else if (we) cnt[6] <= _0527_[6];
  always @(posedge clk, posedge rst)
    if (rst) cnt[7] <= 1'h1;
    else if (we) cnt[7] <= _0527_[7];
  always @(posedge clk)
    if (_0006_)
      if (!_0009_) \mem[1] [0] <= 1'h1;
      else \mem[1] [0] <= wd[0];
  always @(posedge clk)
    if (_0006_)
      if (!_0009_) \mem[1] [1] <= 1'h0;
      else \mem[1] [1] <= wd[1];
  always @(posedge clk)
    if (_0006_)
      if (!_0009_) \mem[1] [2] <= 1'h0;
      else \mem[1] [2] <= wd[2];
  always @(posedge clk)
    if (_0006_)
      if (!_0009_) \mem[1] [3] <= 1'h0;
      else \mem[1] [3] <= wd[3];
  always @(posedge clk)
    if (_0006_)
      if (!_0009_) \mem[1] [4] <= 1'h0;
      else \mem[1] [4] <= wd[4];
  always @(posedge clk)
    if (_0006_)
      if (!_0009_) \mem[1] [5] <= 1'h0;
      else \mem[1] [5] <= wd[5];
  always @(posedge clk)
    if (_0006_)
      if (!_0009_) \mem[1] [6] <= 1'h0;
      else \mem[1] [6] <= wd[6];
  always @(posedge clk)
    if (_0006_)
      if (!_0009_) \mem[1] [7] <= 1'h0;
      else \mem[1] [7] <= wd[7];
  always @(posedge clk)
    if (_0002_)
      if (!_0013_) \mem[5] [0] <= 1'h1;
      else \mem[5] [0] <= wd[0];
  always @(posedge clk)
    if (_0002_)
      if (!_0013_) \mem[5] [1] <= 1'h0;
      else \mem[5] [1] <= wd[1];
  always @(posedge clk)
    if (_0002_)
      if (!_0013_) \mem[5] [2] <= 1'h1;
      else \mem[5] [2] <= wd[2];
  always @(posedge clk)
    if (_0002_)
      if (!_0013_) \mem[5] [3] <= 1'h0;
      else \mem[5] [3] <= wd[3];
  always @(posedge clk)
    if (_0002_)
      if (!_0013_) \mem[5] [4] <= 1'h0;
      else \mem[5] [4] <= wd[4];
  always @(posedge clk)
    if (_0002_)
      if (!_0013_) \mem[5] [5] <= 1'h0;
      else \mem[5] [5] <= wd[5];
  always @(posedge clk)
    if (_0002_)
      if (!_0013_) \mem[5] [6] <= 1'h0;
      else \mem[5] [6] <= wd[6];
  always @(posedge clk)
    if (_0002_)
      if (!_0013_) \mem[5] [7] <= 1'h0;
      else \mem[5] [7] <= wd[7];
  always @(posedge clk)
    if (_0004_)
      if (!_0011_) \mem[3] [0] <= 1'h1;
      else \mem[3] [0] <= wd[0];
  always @(posedge clk)
    if (_0004_)
      if (!_0011_) \mem[3] [1] <= 1'h1;
      else \mem[3] [1] <= wd[1];
  always @(posedge clk)
    if (_0004_)
      if (!_0011_) \mem[3] [2] <= 1'h0;
      else \mem[3] [2] <= wd[2];
  always @(posedge clk)
    if (_0004_)
      if (!_0011_) \mem[3] [3] <= 1'h0;
      else \mem[3] [3] <= wd[3];
  always @(posedge clk)
    if (_0004_)
      if (!_0011_) \mem[3] [4] <= 1'h0;
      else \mem[3] [4] <= wd[4];
  always @(posedge clk)
    if (_0004_)
      if (!_0011_) \mem[3] [5] <= 1'h0;
      else \mem[3] [5] <= wd[5];
  always @(posedge clk)
    if (_0004_)
      if (!_0011_) \mem[3] [6] <= 1'h0;
      else \mem[3] [6] <= wd[6];
  always @(posedge clk)
    if (_0004_)
      if (!_0011_) \mem[3] [7] <= 1'h0;
      else \mem[3] [7] <= wd[7];
  always @(posedge clk)
    if (_0005_)
      if (!_0010_) \mem[2] [0] <= 1'h0;
      else \mem[2] [0] <= wd[0];
  always @(posedge clk)
    if (_0005_)
      if (!_0010_) \mem[2] [1] <= 1'h1;
      else \mem[2] [1] <= wd[1];
  always @(posedge clk)
    if (_0005_)
      if (!_0010_) \mem[2] [2] <= 1'h0;
      else \mem[2] [2] <= wd[2];
  always @(posedge clk)
    if (_0005_)
      if (!_0010_) \mem[2] [3] <= 1'h0;
      else \mem[2] [3] <= wd[3];
  always @(posedge clk)
    if (_0005_)
      if (!_0010_) \mem[2] [4] <= 1'h0;
      else \mem[2] [4] <= wd[4];
  always @(posedge clk)
    if (_0005_)
      if (!_0010_) \mem[2] [5] <= 1'h0;
      else \mem[2] [5] <= wd[5];
  always @(posedge clk)
    if (_0005_)
      if (!_0010_) \mem[2] [6] <= 1'h0;
      else \mem[2] [6] <= wd[6];
  always @(posedge clk)
    if (_0005_)
      if (!_0010_) \mem[2] [7] <= 1'h0;
      else \mem[2] [7] <= wd[7];
  always @*
    if (g) lo[0] = li[0];
  always @*
    if (g) lo[1] = li[1];
  always @*
    if (g) lo[2] = li[2];
  always @*
    if (g) lo[3] = li[3];
  always @*
    if (g) lo[4] = li[4];
  always @*
    if (g) lo[5] = li[5];
  always @*
    if (g) lo[6] = li[6];
  always @*
    if (g) lo[7] = li[7];
  always @(posedge clk)
    if (_0000_)
      if (!_0015_) \mem[7] [0] <= 1'h1;
      else \mem[7] [0] <= wd[0];
  always @(posedge clk)
    if (_0000_)
      if (!_0015_) \mem[7] [1] <= 1'h1;
      else \mem[7] [1] <= wd[1];
  always @(posedge clk)
    if (_0000_)
      if (!_0015_) \mem[7] [2] <= 1'h1;
      else \mem[7] [2] <= wd[2];
  always @(posedge clk)
    if (_0000_)
      if (!_0015_) \mem[7] [3] <= 1'h0;
      else \mem[7] [3] <= wd[3];
  always @(posedge clk)
    if (_0000_)
      if (!_0015_) \mem[7] [4] <= 1'h0;
      else \mem[7] [4] <= wd[4];
  always @(posedge clk)
    if (_0000_)
      if (!_0015_) \mem[7] [5] <= 1'h0;
      else \mem[7] [5] <= wd[5];
  always @(posedge clk)
    if (_0000_)
      if (!_0015_) \mem[7] [6] <= 1'h0;
      else \mem[7] [6] <= wd[6];
  always @(posedge clk)
    if (_0000_)
      if (!_0015_) \mem[7] [7] <= 1'h0;
      else \mem[7] [7] <= wd[7];
  always @(posedge clk)
    if (_0001_)
      if (!_0014_) \mem[6] [0] <= 1'h0;
      else \mem[6] [0] <= wd[0];
  always @(posedge clk)
    if (_0001_)
      if (!_0014_) \mem[6] [1] <= 1'h1;
      else \mem[6] [1] <= wd[1];
  always @(posedge clk)
    if (_0001_)
      if (!_0014_) \mem[6] [2] <= 1'h1;
      else \mem[6] [2] <= wd[2];
  always @(posedge clk)
    if (_0001_)
      if (!_0014_) \mem[6] [3] <= 1'h0;
      else \mem[6] [3] <= wd[3];
  always @(posedge clk)
    if (_0001_)
      if (!_0014_) \mem[6] [4] <= 1'h0;
      else \mem[6] [4] <= wd[4];
  always @(posedge clk)
    if (_0001_)
      if (!_0014_) \mem[6] [5] <= 1'h0;
      else \mem[6] [5] <= wd[5];
  always @(posedge clk)
    if (_0001_)
      if (!_0014_) \mem[6] [6] <= 1'h0;
      else \mem[6] [6] <= wd[6];
  always @(posedge clk)
    if (_0001_)
      if (!_0014_) \mem[6] [7] <= 1'h0;
      else \mem[6] [7] <= wd[7];
  always @(posedge clk)
    if (_0007_)
      if (!_0008_) \mem[0] [0] <= 1'h0;
      else \mem[0] [0] <= wd[0];
  always @(posedge clk)
    if (_0007_)
      if (!_0008_) \mem[0] [1] <= 1'h0;
      else \mem[0] [1] <= wd[1];
  always @(posedge clk)
    if (_0007_)
      if (!_0008_) \mem[0] [2] <= 1'h0;
      else \mem[0] [2] <= wd[2];
  always @(posedge clk)
    if (_0007_)
      if (!_0008_) \mem[0] [3] <= 1'h0;
      else \mem[0] [3] <= wd[3];
  always @(posedge clk)
    if (_0007_)
      if (!_0008_) \mem[0] [4] <= 1'h0;
      else \mem[0] [4] <= wd[4];
  always @(posedge clk)
    if (_0007_)
      if (!_0008_) \mem[0] [5] <= 1'h0;
      else \mem[0] [5] <= wd[5];
  always @(posedge clk)
    if (_0007_)
      if (!_0008_) \mem[0] [6] <= 1'h0;
      else \mem[0] [6] <= wd[6];
  always @(posedge clk)
    if (_0007_)
      if (!_0008_) \mem[0] [7] <= 1'h0;
      else \mem[0] [7] <= wd[7];
  always @(posedge clk)
    if (_0003_)
      if (!_0012_) \mem[4] [0] <= 1'h0;
      else \mem[4] [0] <= wd[0];
  always @(posedge clk)
    if (_0003_)
      if (!_0012_) \mem[4] [1] <= 1'h0;
      else \mem[4] [1] <= wd[1];
  always @(posedge clk)
    if (_0003_)
      if (!_0012_) \mem[4] [2] <= 1'h1;
      else \mem[4] [2] <= wd[2];
  always @(posedge clk)
    if (_0003_)
      if (!_0012_) \mem[4] [3] <= 1'h0;
      else \mem[4] [3] <= wd[3];
  always @(posedge clk)
    if (_0003_)
      if (!_0012_) \mem[4] [4] <= 1'h0;
      else \mem[4] [4] <= wd[4];
  always @(posedge clk)
    if (_0003_)
      if (!_0012_) \mem[4] [5] <= 1'h0;
      else \mem[4] [5] <= wd[5];
  always @(posedge clk)
    if (_0003_)
      if (!_0012_) \mem[4] [6] <= 1'h0;
      else \mem[4] [6] <= wd[6];
  always @(posedge clk)
    if (_0003_)
      if (!_0012_) \mem[4] [7] <= 1'h0;
      else \mem[4] [7] <= wd[7];
  assign _0527_[0] = _0526_[0];
  assign bs[7:5] = 3'h0;
endmodule
