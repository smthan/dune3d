#include "solid_model_util.hpp"
#include "util/paths.hpp"
#include "document/entity/entity.hpp"
#include "document/entity/entity_line2d.hpp"
#include "document/entity/entity_arc2d.hpp"
#include "document/entity/entity_circle2d.hpp"
#include "document/entity/entity_bezier2d.hpp"
#include "document/entity/entity_workplane.hpp"
#include "document/entity/entity_cluster.hpp"
#include "document/entity/entity_text.hpp"
#include "document/document.hpp"
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Pnt.hxx>
#include <NCollection_Array1.hxx>
#include <Geom_BezierCurve.hxx>
#include <gp_Circ.hxx>

namespace dune3d::solid_model_util {


FaceBuilder::FaceBuilder()
{
    m_builder.MakeCompound(m_compound);
}

struct VertexInfo {

    bool sub;
    unsigned int vertex_index;
    unsigned int path_index;

    static constexpr unsigned int mask = (1u << 31) - 1;

    constexpr int64_t pack() const
    {
        assert(vertex_index <= mask);
        assert(path_index <= mask);
        return sub | ((vertex_index & mask) << 1) | (static_cast<uint64_t>(path_index & mask) << (31 + 1));
    }

    int64_t get_vertex_index() const
    {
        return sub | (vertex_index << 1);
    }

    static constexpr VertexInfo unpack(int64_t z)
    {
        auto s = static_cast<bool>(z & 1);
        auto v = static_cast<unsigned int>((z >> 1) & (mask));
        auto p = static_cast<unsigned int>((z >> (31 + 1)) & (mask));
        return {s, v, p};
    }

    static constexpr int64_t make_z(unsigned int path_index, unsigned int vertex_index, unsigned int i,
                                    unsigned int segments)
    {
        return VertexInfo{.sub = i > segments / 2, .vertex_index = vertex_index, .path_index = path_index}.pack();
    }
};

static double angle(const glm::dvec2 &v)
{
    return glm::atan(v.y, v.x);
}

template <typename T> T c2pi(T x)
{
    while (x < 0)
        x += 2 * M_PI;

    while (x > 2 * M_PI)
        x -= 2 * M_PI;
    return x;
}

template <typename T> T c2pim(T x)
{
    while (x < -2 * M_PI)
        x += 2 * M_PI;

    while (x > 0)
        x -= 2 * M_PI;
    return x;
}

static glm::dvec2 euler(double r, double phi)
{
    return {r * cos(phi), r * sin(phi)};
}

static Clipper2Lib::PathD path_to_clipper(const Path &path, unsigned int path_index)
{
    Clipper2Lib::PathD cpath;
    cpath.reserve(path.size());
    for (size_t iv = 0; iv < path.size(); iv++) {
        auto &[node, edge] = path.at(iv);
        if (auto circle = dynamic_cast<const EntityCircle2D *>(&edge.entity)) {
            {
                const unsigned int segments = 64;

                float dphi = 2 * M_PI;
                dphi /= segments;
                float a = 0;
                for (unsigned int i = 0; i < segments; i++) {
                    const auto p0 = edge.transform(circle->m_center + euler(circle->m_radius, a));
                    cpath.emplace_back(p0.x, p0.y, VertexInfo::make_z(path_index, iv, i, segments));
                    a += dphi;
                }
            }
            break;
        }


        auto pt = node.get_pt_for_edge(edge);
        auto pc = Paths::get_pt(edge.entity, pt, nullptr);


        if (auto arc = dynamic_cast<const EntityArc2D *>(&edge.entity)) {
            const auto radius0 = glm::length(arc->m_center - arc->m_from);
            const auto a0 = c2pi(angle(pc - arc->m_center));
            const auto a1 = c2pi(angle(Paths::get_pt(edge.entity, pt == 1 ? 2 : 1, nullptr) - arc->m_center));
            const unsigned int segments = 64;

            float dphi = c2pi(a1 - a0);
            if (pt == 2) {
                dphi = c2pim(a1 - a0);
            }
            if (std::abs(dphi) < 1e-2)
                dphi = 2 * M_PI;

            dphi /= segments;
            float a = a0;
            for (unsigned int i = 0; i < segments; i++) {
                const auto p0 = edge.transform(arc->m_center + euler(radius0, a));
                cpath.emplace_back(p0.x, p0.y, VertexInfo::make_z(path_index, iv, i, segments));
                a += dphi;
            }
        }
        else if (auto bezier = dynamic_cast<const EntityBezier2D *>(&edge.entity)) {
            const bool forward = pt == 1;
            const unsigned int segments = 64;

            for (unsigned int i = 0; i < segments; i++) {
                auto t = (double)i / segments;
                if (!forward)
                    t = 1 - t;
                const auto p0 = edge.transform(bezier->get_interpolated(t));
                cpath.emplace_back(p0.x, p0.y, VertexInfo::make_z(path_index, iv, i, segments));
            }
        }
        else {
            const auto pct = edge.transform({pc.x, pc.y});
            cpath.emplace_back(
                    pct.x, pct.y,
                    VertexInfo{.sub = false, .vertex_index = static_cast<unsigned int>(iv), .path_index = path_index}
                            .pack());
        }
    }

    return cpath;
}


static bool path_is_valid(const Path &path)
{
    if (path.size() >= 3)
        return true;
    if (path.size() == 1 && path.front().second.entity.get_type() == Entity::Type::CIRCLE_2D)
        return true;
    for (auto &p : path) {
        if (p.second.entity.of_type(Entity::Type::ARC_2D, Entity::Type::BEZIER_2D))
            return true;
    }

    return false;
}


bool FaceBuilder::check_path(const Clipper2Lib::PathD &contour)
{
    const auto path_index = VertexInfo::unpack(contour.front().z).path_index;
    if (contour.front().z == -1)
        return false;

    auto same_z = std::ranges::all_of(contour, [path_index](auto &pt) {
        return VertexInfo::unpack(pt.z).path_index == path_index && pt.z != -1;
    });
    if (!same_z)
        return false;

    return true;
}

void FaceBuilder::visit_poly_path(const Clipper2Lib::PolyPathD &path, const DocumentRef &docref)
{
    assert(!path.IsHole());

    auto &contour = path.Polygon();
    if (!check_path(contour))
        return;

    auto wire = path_to_wire(contour, false, docref);
    m_wires.push_back(wire);
    BRepBuilderAPI_MakeFace make_face(wire);

    for (auto &child : path) {
        auto &hole = *child;
        assert(hole.IsHole());

        auto &hole_contour = hole.Polygon();
        if (check_path(hole_contour)) {
            auto hole_wire = path_to_wire(hole_contour, true, docref);
            m_has_hole = true;
            make_face.Add(hole_wire);
        }
        for (auto &child2 : hole) {
            visit_poly_path(*child2, docref);
        }
    }

    m_n_faces++;
    m_builder.Add(m_compound, make_face.Face());
}

const TopoDS_Compound &FaceBuilder::get_faces() const
{
    return m_compound;
}

unsigned int FaceBuilder::get_n_faces() const
{
    return m_n_faces;
}

static std::pair<const Node &, const Edge &> get_node_and_edge(const Path &path, size_t i, bool reverse)
{
    if (!reverse)
        return path.at(i);

    i = path.size() - i - 1;
    size_t iprev = i - 1;
    if (i == 0)
        iprev = path.size() - 1;
    return {path.at(i).first, path.at(iprev).second};
}

TopoDS_Wire FaceBuilder::path_to_wire(const Clipper2Lib::PathD &path, bool hole, const DocumentRef &docref)
{

    const Path &orig_path = docref.paths.paths.at(VertexInfo::unpack(path.front().z).path_index);
    if (orig_path.size() == 1) {
        auto &orig_edge = orig_path.front().second;
        if (auto rad = dynamic_cast<const IEntityRadius *>(&orig_edge.entity)) {
            BRepBuilderAPI_MakeWire wire;

            const auto center = docref.transform(docref.wrkpl.transform(orig_edge.transform(rad->get_center())));
            auto normal = docref.transform_normal(docref.wrkpl.get_normal_vector());
            if (hole)
                normal *= -1;

            gp_Circ garc(gp_Ax2(gp_Pnt(center.x, center.y, center.z), gp_Dir(normal.x, normal.y, normal.z)),
                         rad->get_radius());


            auto edge = BRepBuilderAPI_MakeEdge(garc);
            wire.Add(edge);

            return wire;
        }
    }


    assert(path.size() >= 3);
    // see if we're traversing orig_path in reverse

    const auto sz = path.size();

    unsigned int pos_count = 0;
    unsigned int neg_count = 0;

    for (size_t i = 0; i < sz; i++) {
        auto inext = (i + 1) % sz;
        auto vi = VertexInfo::unpack(path.at(i).z);
        auto vinext = VertexInfo::unpack(path.at(inext).z);
        auto delta = vinext.get_vertex_index() - vi.get_vertex_index();
        if (delta > 0)
            pos_count++;
        else if (delta < 0)
            neg_count++;
        if (pos_count + neg_count >= 3)
            break;
    }
    const bool path_reverse = neg_count > pos_count;

    BRepBuilderAPI_MakeWire wire;


    for (size_t i = 0; i < orig_path.size(); i++) {
        const auto &[node, edge] = get_node_and_edge(orig_path, i, path_reverse);
        const auto pt = node.get_pt_for_edge(edge);
        const auto next_pt = pt == 1 ? 2 : 1;
        const auto pa = Paths::get_pt(edge.entity, pt, edge.transform_fn);
        const auto pb = Paths::get_pt(edge.entity, next_pt, edge.transform_fn);

        const auto pat = docref.transform(docref.wrkpl.transform(pa));
        const auto pbt = docref.transform(docref.wrkpl.transform(pb));

        if (auto arc = dynamic_cast<const EntityArc2D *>(&edge.entity)) {
            auto normal = docref.transform_normal(docref.wrkpl.get_normal_vector());

            gp_Pnt sa(pat.x, pat.y, pat.z);
            gp_Pnt ea(pbt.x, pbt.y, pbt.z);
            const auto center = docref.transform(docref.wrkpl.transform(edge.transform(arc->m_center)));
            const auto radius = arc->get_radius();
            if (pt == 2)
                normal *= -1;

            gp_Circ garc(gp_Ax2(gp_Pnt(center.x, center.y, center.z), gp_Dir(normal.x, normal.y, normal.z)), radius);

            auto new_edge = BRepBuilderAPI_MakeEdge(garc, sa, ea);
            wire.Add(new_edge);
        }
        else if (auto bezier = dynamic_cast<const EntityBezier2D *>(&edge.entity)) {
            TColgp_Array1OfPnt poles(1, 4);
            const auto control_pta = pt == 1 ? 3 : 4;
            const auto control_ptb = pt == 1 ? 4 : 3;

            const auto cat = docref.transform(
                    docref.wrkpl.transform(edge.transform(bezier->get_point_in_workplane(control_pta))));
            const auto cbt = docref.transform(
                    docref.wrkpl.transform(edge.transform(bezier->get_point_in_workplane(control_ptb))));

            poles(1) = gp_Pnt(pat.x, pat.y, pat.z);
            poles(2) = gp_Pnt(cat.x, cat.y, cat.z);
            poles(3) = gp_Pnt(cbt.x, cbt.y, cbt.z);
            poles(4) = gp_Pnt(pbt.x, pbt.y, pbt.z);

            Handle(Geom_BezierCurve) curve = new Geom_BezierCurve(poles);
            auto new_edge = BRepBuilderAPI_MakeEdge(curve);

            wire.Add(new_edge);
        }
        else {
            auto new_edge = BRepBuilderAPI_MakeEdge(gp_Pnt(pat.x, pat.y, pat.z), gp_Pnt(pbt.x, pbt.y, pbt.z));

            wire.Add(new_edge);
        }
    }
    return wire;
}

FaceBuilder FaceBuilder::from_document(const Document &doc, const UUID &wrkpl_uu, const UUID &source_group_uu,
                                       Transform fn_transform, Transform fn_transform_normal)
{
    auto paths = Paths::from_document(doc, wrkpl_uu, source_group_uu);

    // have node/edge paths, now turn them into clipper paths
    Clipper2Lib::PathsD cpaths;
    cpaths.reserve(paths.paths.size());
    {
        unsigned int path_index = 0;
        for (auto &path : paths.paths) {
            if (path_is_valid(path))
                cpaths.emplace_back(path_to_clipper(path, path_index++));
        }
    }
    Clipper2Lib::PolyTreeD poly_tree;
    Clipper2Lib::ClipperD clipper{3};
    clipper.AddSubject(cpaths);
    if (0) {
        std::ofstream ofs("/tmp/paths.txt");
        for (auto &path : cpaths) {
            for (auto &p : path) {
                ofs << p.x << " " << p.y << " " << p.z << std::endl;
            }
            ofs << path.front().x << " " << path.front().y << " " << path.front().z << std::endl;
            ofs << std::endl;
        }
    }
    clipper.SetZCallback([](const Clipper2Lib::PointD &e1bot, const Clipper2Lib::PointD &e1top,
                            const Clipper2Lib::PointD &e2bot, const Clipper2Lib::PointD &e2top,
                            Clipper2Lib::PointD &pt) { pt.z = -1; });

    clipper.Execute(Clipper2Lib::ClipType::Union, Clipper2Lib::FillRule::EvenOdd, poly_tree);

    auto &wrkpl = doc.get_entity<EntityWorkplane>(wrkpl_uu);

    FaceBuilder face_builder{};

    for (auto &child : poly_tree) {
        face_builder.visit_poly_path(*child, DocumentRef{wrkpl, paths, fn_transform, fn_transform_normal});
    }

    return face_builder;
}

FaceBuilder FaceBuilder::from_document(const Document &doc, const UUID &wrkpl_uu, const UUID &source_group_uu,
                                       const glm::dvec3 &offset)
{
    return from_document(
            doc, wrkpl_uu, source_group_uu, [offset](const glm::dvec3 &p) { return p + offset; },
            [](const glm::dvec3 &p) { return p; });
}

} // namespace dune3d::solid_model_util
