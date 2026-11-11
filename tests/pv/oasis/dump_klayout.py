# The same canonical dump from klayout.
import sys, klayout.db as db
ly = db.Layout(); ly.read(sys.argv[1])
lines = ["dbu %.9g" % ly.dbu]
def norm(pts):
    p = list(pts)
    ch = True
    while ch and len(p) >= 3:
        ch = False
        for i in range(len(p)):
            a, b, c = p[i-1], p[i], p[(i+1) % len(p)]
            cr = (b[0]-a[0])*(c[1]-b[1]) - (b[1]-a[1])*(c[0]-b[0])
            if b == c or cr == 0:
                del p[i]; ch = True; break
    if len(p) < 3: return []
    area = sum(p[i][0]*p[(i+1)%len(p)][1] - p[(i+1)%len(p)][0]*p[i][1] for i in range(len(p)))
    if area < 0: p.reverse()
    m = min(range(len(p)), key=lambda i: p[i])
    return p[m:] + p[:m]
for cell in ly.each_cell():
    for li in ly.layer_indexes():
        info = ly.get_info(li)
        lay = "%d/%d" % (info.layer, info.datatype)
        for s in cell.shapes(li).each():
            if s.is_path():
                p = s.path
                if p.num_points() == 1 and p.round:
                    lines.append("%s circle %s" % (cell.name, lay)); continue
                lines.append("%s path %s w%d e%d,%d %s" % (cell.name, lay, p.width, p.bgn_ext, p.end_ext, " ".join("%d,%d" % (q.x, q.y) for q in p.each_point())))
            elif s.is_text():
                t = s.text
                lines.append("%s text %s %d,%d %s" % (cell.name, lay, t.x, t.y, t.string))
            elif s.is_polygon() or s.is_box() or s.is_simple_polygon():
                poly = s.polygon
                if poly.num_points_hull() >= 60 and len(sys.argv) < 3:
                    lines.append("%s circle %s" % (cell.name, lay)); continue
                q = norm([(pt.x, pt.y) for pt in poly.each_point_hull()])
                lines.append("%s poly %s%s" % (cell.name, lay, "".join(" %d,%d" % v for v in q)))
    for inst in cell.each_inst():
        tr = inst.cplx_trans if inst.is_complex() else db.ICplxTrans(inst.trans)
        na, nb = max(1, inst.na), max(1, inst.nb)
        for j in range(nb):
            for i in range(na):
                x = tr.disp.x + i*inst.a.x + j*inst.b.x; y = tr.disp.y + i*inst.a.y + j*inst.b.y
                lines.append("%s ref %s %d,%d a%.6g m%d x%.6g" % (cell.name, ly.cell(inst.cell_index).name, x, y, tr.angle % 360, 1 if tr.is_mirror() else 0, tr.mag))
lines.sort()
print("\n".join(lines))
