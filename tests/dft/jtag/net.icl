// A two-level SIB network with a sensor instrument.
Module SIB {
  ScanInPort si;
  ScanInPort fso;
  ScanOutPort so { Source sr; }
  SelectPort sel; ShiftEnPort se; CaptureEnPort ce; UpdateEnPort ue; TCKPort tck; ResetPort rst;
  ToSelectPort tsel;
  ScanRegister sr { ScanInSource mux; ResetValue 1'b0; }
  ScanMux mux SelectedBy sr { 1'b0 : si; 1'b1 : fso; }
}
Module TDR {
  Parameter N = 8;
  ScanInPort si;
  ScanOutPort so { Source R[0]; }
  DataInPort din[$N-1:0];
  DataOutPort dout[$N-1:0] { Source R; }
  ScanRegister R[$N-1:0] { ScanInSource si; CaptureSource din; ResetValue 'b0; }
}
Module Sensor {
  DataInPort en;
  DataInPort mode[1:0];
  DataOutPort temp[4:0];
  DataOutPort done;
}
Module Sub {
  ScanInPort si;
  ScanOutPort so { Source sib.so; }
  Instance sib Of SIB { InputPort si = si; InputPort fso = cfg; }
  ScanRegister cfg[7:0] { ScanInSource si; ResetValue 8'h3C; RefEnum Modes; }
  Enum Modes { idle = 8'h00; fast = 8'hF0; slow = 8'h0F; }
  Alias low[3:0] = cfg[3:0];
}
Module Top {
  ScanInPort si;
  ScanOutPort so { Source sib1.so; }
  SelectPort sel; ShiftEnPort se; CaptureEnPort ce; UpdateEnPort ue; TCKPort tck; ResetPort rst;
  DataInPort strap[1:0];
  DataOutPort status[1:0] { Source tdr1.dout[7:6]; }
  Instance sib1 Of SIB { InputPort si = sib2.so; InputPort fso = tdr1.so; }
  Instance tdr1 Of TDR { Parameter N = 8; InputPort si = sib2.so; InputPort din = sens.done, sens.temp, strap; }
  Instance sens Of Sensor { InputPort en = tdr1.dout[0]; InputPort mode = tdr1.dout[2:1]; }
  Instance sib2 Of SIB { InputPort si = si; InputPort fso = sub.so; }
  Instance sub Of Sub { InputPort si = si; }
}
