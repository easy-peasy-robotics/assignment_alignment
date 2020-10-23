/*
 * Copyright (C) 2020 iCub Tech Facility - Istituto Italiano di Tecnologia
 * Author: Ugo Pattacini <ugo.pattacini@iit.it>
 * CopyPolicy: Released under the terms of the GNU GPL v3.0.
*/

#include <string>
#include <cmath>
#include <fstream>

#include <robottestingframework/dll/Plugin.h>
#include <robottestingframework/TestAssert.h>

#include <yarp/robottestingframework/TestCase.h>
#include <yarp/os/all.h>
#include <yarp/pcl/Pcl.h>

#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/features/normal_3d.h>
#include <pcl/conversions.h>
#include <pcl/common/transforms.h>
#include <pcl/kdtree/kdtree_flann.h>

using namespace std;
using namespace robottestingframework;
using namespace yarp::os;

/**********************************************************************/
class TestAssignmentModelAlignment : public yarp::robottestingframework::TestCase
{
    RpcClient visionPort;
    BufferedPort<Bottle> objectPort;

    pcl::PointCloud<pcl::PointXYZRGBA>::Ptr pc_object_pcl;
    pcl::PointCloud<pcl::PointXYZRGBA>::Ptr pc_transformed_pcl;

public:
    /******************************************************************/
    TestAssignmentModelAlignment() :
        yarp::robottestingframework::TestCase("TestAssignmentModelAlignment") {}

    /******************************************************************/
    virtual ~TestAssignmentModelAlignment() {}

    /******************************************************************/
    bool setup(Property& property) override
    {
        float rpcTmo=(float)property.check("rpc-timeout",Value(120.0)).asDouble();

        visionPort.open("/"+getName()+"/vision:rpc");
        visionPort.asPort().setTimeout(rpcTmo);
        if (!Network::connect(visionPort.getName(), "/model-alignment/rpc")) {
            ROBOTTESTINGFRAMEWORK_ASSERT_FAIL("Unable to connect to /model-alignment/rpc");
        }

        objectPort.open("/"+getName()+"/object:o");
        if (!Network::connect(objectPort.getName(), "/assignment_model-alignment-mustard_bottle/mover:i")) {
            ROBOTTESTINGFRAMEWORK_ASSERT_FAIL("Unable to connect to /assignment_model-alignment-mustard_bottle/mover:i");
        }

        pc_object_pcl = make_shared<pcl::PointCloud<pcl::PointXYZRGBA>>();
        pc_transformed_pcl = make_shared<pcl::PointCloud<pcl::PointXYZRGBA>>();

        return true;
    }

    /******************************************************************/
    void tearDown() override
    {
        visionPort.close();
        objectPort.close();
    }

    /******************************************************************/
    void run() override
    {

        yarp::os::ResourceFinder rf;
        rf.setDefaultContext("model-alignment");

        unsigned int score{0};
        {
            Bottle cmd, rep;
            cmd.addString("home");
            if (!visionPort.write(cmd, rep))
            {
                ROBOTTESTINGFRAMEWORK_ASSERT_FAIL("Unable to talk to /model-alignment/rpc");
            }
        }

        {
            Bottle cmd, rep;
            cmd.addString("is_model_valid");
            if (visionPort.write(cmd, rep))
            {
                if (rep.get(0).asVocab() == Vocab::encode("ok"))
                {
                    ROBOTTESTINGFRAMEWORK_TEST_REPORT(Asserter::format("Loaded model correctly"));
                    score+=5;
                }
                else
                {
                    ROBOTTESTINGFRAMEWORK_ASSERT_FAIL("The model was not loaded properly");
                }
            }
            else
            {
                ROBOTTESTINGFRAMEWORK_ASSERT_FAIL("Unable to talk to /model-alignment/rpc");
            }
        }

        const double high{0.0001};
        const double low{0.1};
        const double max_distance{1.0};
        unsigned int max_iterations{2000};
        const double transf_epsilon{1e-8};
        const double fitness_epsilon{1e-9};
        unsigned int response_perc{0};
        for (int attempt = 1; attempt <= 5; attempt++) {
            ROBOTTESTINGFRAMEWORK_TEST_REPORT(Asserter::format("Starting attempt #%d...", attempt));
            double response_score;

            string path = rf.findFile("pose-" + to_string(attempt) + ".ini");
            ROBOTTESTINGFRAMEWORK_TEST_REPORT(Asserter::format("Evaluating %s...", path.c_str()));
            std::fstream file(path, std::ios_base::in);
            double x, y, z, r, p, yw, optimal;
            file >> x >> y >> z >> r >> p >> yw >> optimal;

            Bottle &bPose = objectPort.prepare();
            bPose.clear();
            bPose.addDouble(x);
            bPose.addDouble(y);
            bPose.addDouble(z);
            bPose.addDouble(r);
            bPose.addDouble(p);
            bPose.addDouble(yw);
            objectPort.writeStrict();
            Time::delay(1.0);

            {
                Bottle cmd, rep;
                cmd.addString("align");
                if (visionPort.write(cmd, rep))
                {
                    if (rep.get(0).asVocab() == Vocab::encode("ok"))
                    {
                        cmd.clear();
                        rep.clear();
                        cmd.addString("get_point_clouds");
                        if (visionPort.write(cmd, rep))
                        {
                            Bottle *b = rep.get(0).asList();
                            Bottle *source = b->get(0).asList();
                            Bottle *transformed = b->get(1).asList();
                            yarp::sig::PointCloud<yarp::sig::DataXYZRGBA> pc_object, pc_transformed;
                            pc_object.fromBottle(*source);
                            pc_transformed.fromBottle(*transformed);
                            yarp::pcl::toPCL<yarp::sig::DataXYZRGBA, pcl::PointXYZRGBA>(pc_object, *pc_object_pcl);
                            yarp::pcl::toPCL<yarp::sig::DataXYZRGBA, pcl::PointXYZRGBA>(pc_transformed, *pc_transformed_pcl);
                            response_score = computeScore(pc_transformed_pcl, pc_object_pcl);
                            double result = fabs(response_score - optimal);
                            if (result <= high)
                            {
                                response_perc++;
                                ROBOTTESTINGFRAMEWORK_TEST_REPORT(Asserter::format("Well done aligning"));
                            }
                            if (result > high && result <= low)
                            {
                                ROBOTTESTINGFRAMEWORK_TEST_REPORT(Asserter::format("Almost there with alignment..."));
                            }
                            if (result > low)
                            {
                                ROBOTTESTINGFRAMEWORK_TEST_REPORT(Asserter::format("Epic failure while aligning"));
                            }
                        }
                        else
                        {
                            ROBOTTESTINGFRAMEWORK_ASSERT_FAIL("Unable to talk to /model-alignment/rpc");

                        }
                    }
                }
                else
                {
                    ROBOTTESTINGFRAMEWORK_ASSERT_FAIL("Unable to talk to /model-alignment/rpc");
                }
            }

            Time::delay(2.0);
        }

        response_perc/=5;
        response_perc*=100;
        if (response_perc <= 33)
        {
            score+=0;
        }
        if (response_perc > 33 && response_perc <=66)
        {
            score+=10;
        }
        if (response_perc > 66)
        {
            score+=20;
        }

        {
            std::vector<double> response_params;
            response_params.resize(4);
            Bottle cmd, rep;
            cmd.addString("get_parameters");
            if (visionPort.write(cmd, rep))
            {
                Bottle *reply = rep.get(0).asList();
                response_params[0] = reply->get(0).asDouble();
                response_params[1] = reply->get(1).asDouble();
                response_params[2] = reply->get(2).asDouble();
                response_params[3] = reply->get(3).asDouble();
            }
            else
            {
                ROBOTTESTINGFRAMEWORK_ASSERT_FAIL("Unable to talk to /model-alignment/rpc");
            }

            if (fabs(response_params[0] - max_distance) < 1.0)
            {
                score+=2;
                ROBOTTESTINGFRAMEWORK_TEST_REPORT(Asserter::format("Well done setting maximum correspondence distance"));
            }
            else
            {
                score+=0;
                ROBOTTESTINGFRAMEWORK_TEST_REPORT(Asserter::format("Try to play with maximum correspondence distance"));
            }

            if (fabs(response_params[1] - max_iterations) < 1000)
            {
                score+=2;
                ROBOTTESTINGFRAMEWORK_TEST_REPORT(Asserter::format("Well done setting maximum number of iterations"));
            }
            else
            {
                score+=0;
                ROBOTTESTINGFRAMEWORK_TEST_REPORT(Asserter::format("Try to play with maximum number of iterations"));
            }

            if (fabs(response_params[2] - transf_epsilon) < 1e-8)
            {
                score+=2;
                ROBOTTESTINGFRAMEWORK_TEST_REPORT(Asserter::format("Well done setting the transformation epsilon"));
            }
            else
            {
                score+=0;
                ROBOTTESTINGFRAMEWORK_TEST_REPORT(Asserter::format("Try to play with transformation epsilon"));
            }

            if (fabs(response_params[3] - fitness_epsilon) < 1e-9)
            {
                score+=2;
                ROBOTTESTINGFRAMEWORK_TEST_REPORT(Asserter::format("Well done setting the euclidean distance difference epsilon"));
            }
            else
            {
                score+=0;
                ROBOTTESTINGFRAMEWORK_TEST_REPORT(Asserter::format("Try to play with euclidean distance difference epsilon"));
            }
        }

        ROBOTTESTINGFRAMEWORK_TEST_CHECK(score > 15, Asserter::format("Total score = %d", score));
    }

    /**************************************************************************/
    double computeScore(const pcl::PointCloud<pcl::PointXYZRGBA>::Ptr transformed_cloud,
                        const pcl::PointCloud<pcl::PointXYZRGBA>::Ptr input_cloud)
    {
        pcl::KdTreeFLANN<pcl::PointXYZRGBA> tree;
        tree.setInputCloud(input_cloud);

        double fitness_score = 0.0;
        std::vector<int> nn_indices(1);
        std::vector<float> nn_dists(1);
        // For each point in the source dataset
        int nr = 0;
        for (size_t i = 0; i < transformed_cloud->points.size(); ++i) {
            // Find the nearest neighbor
            int nn = tree.nearestKSearch(transformed_cloud->points[i], 1, nn_indices, nn_dists);
            fitness_score += nn_dists[0];
            nr++;
        }
        if (nr > 0)
            return (fitness_score / nr);
        else
            return (std::numeric_limits<double>::infinity());
    }
};

ROBOTTESTINGFRAMEWORK_PREPARE_PLUGIN(TestAssignmentModelAlignment)
