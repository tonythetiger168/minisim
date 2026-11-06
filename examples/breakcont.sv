// break leaves the innermost loop. continue skips to that loop's next
// iteration: a for-step still runs, and a do-while still tests its condition.
// A guard (g < 8) stops the for-continue case if the step is skipped.
module breakcont;
  logic [3:0] i, j, g, s;

  initial begin
    g = 0;
    for (i = 0; i < 5 && g < 8; i = i + 1) begin
      g = g + 1;
      if (i == 2) break;
      $display("FOR_BRK %b", i);
    end
    $display("FOR_BRK_I %b", i);
    $display("FOR_BRK_G %b", g);

    g = 0;
    s = 0;
    for (i = 0; i < 4 && g < 8; i = i + 1) begin
      g = g + 1;
      if (i == 1) continue;
      $display("FOR_CONT %b", i);
      s = s + 1;
    end
    $display("FOR_CONT_I %b", i);
    $display("FOR_CONT_G %b", g);
    $display("FOR_CONT_S %b", s);

    i = 0;
    while (i < 5) begin
      if (i == 2) break;
      $display("WH_BRK %b", i);
      i = i + 1;
    end
    $display("WH_BRK_I %b", i);

    i = 0;
    while (i < 4) begin
      i = i + 1;
      if (i == 2) continue;
      $display("WH_CONT %b", i);
    end
    $display("WH_CONT_I %b", i);

    for (i = 0; i < 3; i = i + 1) begin
      for (j = 0; j < 3; j = j + 1) begin
        if (j == 1) break;
        $display("NEST_BRK %b %b", i, j);
      end
    end

    for (i = 0; i < 2; i = i + 1) begin
      for (j = 0; j < 3; j = j + 1) begin
        if (j == 1) continue;
        $display("NEST_CONT %b %b", i, j);
      end
    end

    g = 0;
    s = 0;
    for (i = 0; i < 4 && g < 8; i = i + 1) begin
      g = g + 1;
      case (i)
        4'd1: continue;
        4'd2: break;
        default: begin
          $display("CASE %b", i);
          s = s + 1;
        end
      endcase
    end
    $display("CASE_I %b", i);
    $display("CASE_G %b", g);
    $display("CASE_S %b", s);

    i = 0;
    do begin
      if (i == 2) break;
      $display("DO_BRK %b", i);
      i = i + 1;
    end while (i < 5);
    $display("DO_BRK_I %b", i);

    i = 0;
    do begin
      if (i == 1) begin
        i = 4'd10;
        continue;
      end
      $display("DO_CONT %b", i);
      i = i + 1;
    end while (i < 3);
    $display("DO_CONT_I %b", i);

    i = 0;
    g = 0;
    forever begin
      g = g + 1;
      if (g == 8) break;
      if (i == 3) break;
      $display("FV_BRK %b", i);
      i = i + 1;
    end
    $display("FV_BRK_I %b", i);
    $display("FV_BRK_G %b", g);

    i = 0;
    g = 0;
    forever begin
      g = g + 1;
      if (g == 8) break;
      i = i + 1;
      if (i == 2) continue;
      $display("FV_CONT %b", i);
      if (i == 4) break;
    end
    $display("FV_CONT_I %b", i);
    $display("FV_CONT_G %b", g);

    i = 0;
    repeat (4) begin
      $display("RP_BRK %b", i);
      if (i == 1) break;
      i = i + 1;
    end
    $display("RP_BRK_I %b", i);

    i = 0;
    repeat (4) begin
      i = i + 1;
      if (i == 2) continue;
      $display("RP_CONT %b", i);
    end
    $display("RP_CONT_I %b", i);

    $finish;
  end
endmodule
