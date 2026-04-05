#include <iostream>
#include <fstream>
#include <sstream>
#include <string>

int main(int argc,char** argv)
{
    if(argc<2)
    {
        std::cout<<"usage: extract_l2 omega.log\n";
        return 0;
    }

    std::ifstream in(argv[1]);
    std::ofstream out("l2_book.csv");

    std::string line;

    uint64_t ts=0;
    double bid_vol=0;
    double ask_vol=0;

    while(std::getline(in,line))
    {
        if(line.find("timestamp=")!=std::string::npos)
        {
            std::stringstream ss(line);
            std::string tok;

            while(ss>>tok)
            {
                if(tok.find("timestamp=")!=std::string::npos)
                {
                    ts = std::stoull(tok.substr(10));
                }
            }
        }

        if(line.find("bid_vol=")!=std::string::npos)
        {
            std::stringstream ss(line);
            std::string tok;

            while(ss>>tok)
            {
                if(tok.find("bid_vol=")!=std::string::npos)
                    bid_vol = std::stod(tok.substr(8));

                if(tok.find("ask_vol=")!=std::string::npos)
                    ask_vol = std::stod(tok.substr(8));
            }

            out<<ts<<","<<bid_vol<<","<<ask_vol<<"\n";
        }
    }
}
